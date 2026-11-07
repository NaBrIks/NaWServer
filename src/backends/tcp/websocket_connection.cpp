// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "websocket_connection.h"
#include "detail/utf8.h"
#include <NaWServer/request.h>
#include <NaWServer/response.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace NaWServer::tcp::detail
{

	namespace
	{

		HttpLimits snapshotLimits(HttpLimits limits, std::size_t bytes, std::size_t count)
		{
			limits.max_header_bytes = bytes;
			limits.max_headers = count;
			return limits;
		}

		std::uint64_t deadlineAfter(std::uint64_t now, std::uint32_t timeout)
		{
			return now + std::min<std::uint64_t>(timeout, std::numeric_limits<std::uint64_t>::max() - now);
		}

	} // namespace

	class WebSocketConnection::Exchange final : public HttpExchange, public WebSocketExchange
	{
		WebSocketConnection &connection_;
		WebSocketReservation *reservation_;
		WebSocketReadResult message_;
		bool opening_;
		bool read_ = false;

	public:
		Exchange(WebSocketConnection &connection, bool opening, WebSocketReservation *reservation,
		    WebSocketReadResult message = {WebSocketReadStatus::Ignored, {}, false})
		  : connection_(connection), reservation_(reservation), message_(message), opening_(opening)
		{
		}

		HttpRequestHead head() const override
		{
			return connection_.metadata_.head();
		}

		HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const override
		{
			return connection_.metadata_.readHeader(name, destination, capacity);
		}

		BodyReadResult readBody(std::uint8_t *, std::size_t) override
		{
			return {BodyReadStatus::End, 0};
		}

		ResponseTransport responseTransport() override
		{
			// 101 is already complete; an HTTP writer cannot produce another reply.
			return {};
		}

		void *extension(const void *key) const override
		{
			return key == &websocket_exchange_key ? const_cast<WebSocketExchange *>(
				static_cast<const WebSocketExchange *>(this)) : nullptr;
		}

		bool opening() const override
		{
			return opening_;
		}

		WebSocketReservation *preparedAdmission() override
		{
			return reservation_;
		}

		std::shared_ptr<WebSocketSession> createSession(const WebSocketLimits &) override
		{
			// This path requires its pre-101 prepared session and admission node.
			return {};
		}

		WebSocketClient *client() const override
		{
			return connection_.client_;
		}

		void attach(WebSocketClient *client) override
		{
			connection_.client_ = client;
		}

		WebSocketReadResult readMessage(const WebSocketLimits &) override
		{
			if(opening_ || read_)
				return {WebSocketReadStatus::Ignored, {}, false};

			read_ = true;
			return message_;
		}

		void close() override
		{
			if(connection_.deferred_callbacks_)
				connection_.application_close_requested_.store(true, std::memory_order_release);
			else
				connection_.requestClose();
		}

	};

	WebSocketConnection::WebSocketConnection(ConnectionRegistry &registry, WebSocketTxPool &pool, Token token,
	    HandlerBase &handler,
	    WebSocketEndpoint &endpoint, std::string_view raw_head, std::size_t header_count,
	    HttpLimits limits, std::uint32_t send_timeout_ms, std::uint32_t close_timeout_ms,
	    std::size_t close_drain_bytes)
	  : registry_(registry), pool_(pool), token_(token), handler_(handler), endpoint_(endpoint),
	    limits_(endpoint.limits()),
	    send_timeout_(send_timeout_ms), close_timeout_(close_timeout_ms), close_drain_bytes_(close_drain_bytes),
	    receive_storage_(pool.memoryPool(), limits_.max_message_size),
	    decoder_(receive_storage_.get(), limits_.max_message_size, limits_.max_message_size),
	    head_storage_(pool.memoryPool(), raw_head.size()),
	    header_entries_(pool.memoryPool(), header_count),
	    metadata_(head_storage_.get(), raw_head.size(), header_entries_.get(), header_count,
	        snapshotLimits(limits, raw_head.size(), header_count)),
	    pending_(pool.memoryPool(), limits_.max_pending_messages)
	{
		if(!receive_storage_ || !head_storage_ || !header_entries_ || !pending_)
			return;

		std::memcpy(head_storage_.get(), raw_head.data(), raw_head.size());
		valid_ = metadata_.advance(raw_head.size()).state == http::wire::ParseStatus::Complete;
	}

	std::shared_ptr<WebSocketConnection> WebSocketConnection::create(ConnectionRegistry &registry,
	    WebSocketTxPool &pool, Token token,
	    HandlerBase &handler, WebSocketEndpoint &endpoint, const http::wire::RequestParser &request,
	    const HttpLimits &limits, std::uint32_t send_timeout_ms, std::uint32_t close_timeout_ms,
	    std::size_t close_drain_bytes)
	{
		const auto raw = request.rawHead();
		const auto maximum = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
		if(!pool.valid() || endpoint.limits().max_message_size > pool.blockSize() ||
		    !token.valid() || raw.empty() || raw.size() > limits.max_header_bytes ||
		    !endpoint.limits().max_message_size || endpoint.limits().max_message_size > maximum ||
		    !endpoint.limits().max_pending_messages || !endpoint.limits().max_pending_bytes ||
		    static_cast<std::size_t>(endpoint.limits().max_pending_messages) > maximum / sizeof(Pending) ||
		    request.headerCount() > maximum / sizeof(http::wire::HeaderEntry) || !send_timeout_ms ||
		    !close_timeout_ms || !close_drain_bytes ||
		    request.head().content_length != 0)
		{
			return {};
		}

		if(!registry.get(token, Owner::Http).valid && !registry.get(token, Owner::WebSocket).valid)
			return {};

		auto *connection = new(std::nothrow) WebSocketConnection(registry, pool, token, handler, endpoint, raw,
		        request.headerCount(), limits, send_timeout_ms, close_timeout_ms, close_drain_bytes);
		if(!connection || !connection->valid_)
		{
			delete connection;
			return {};
		}

		// Admission count is bounded by the registry. This control block still needs
		// heap reserve; global recoverable OOM is not promised on exceptionless IDF.
		return std::shared_ptr<WebSocketConnection>(connection);
	}

	WebSocketConnection::~WebSocketConnection() = default;

	Token WebSocketConnection::token() const
	{
		return token_;
	}

	bool WebSocketConnection::dispatchOpening(WebSocketReservation &reservation)
	{
		if(opened_ || disconnected_ || finished() || !registry_.get(token_, Owner::WebSocket).valid ||
		    reservation.owner() != &endpoint_)
		{
			return false;
		}

		Exchange exchange(*this, true, &reservation);
		Request request(exchange);
		Response response(exchange.responseTransport());
		handler_.handleRequest(&request, response);
		opened_ = reservation.committed() && client_;
		return opened_;
	}

	WebSocketSendResult WebSocketConnection::sendCopy(const std::uint8_t *bytes, std::size_t size, bool binary)
	{
		return sendCopyPolicy(bytes, size, binary, {});
	}

	WebSocketSendResult WebSocketConnection::sendCopyPolicy(const std::uint8_t *bytes, std::size_t size, bool binary,
	    WritePolicy policy)
	{
		if(!bytes && size)
			return WebSocketSendResult::InvalidArgument;

		if(size > limits_.max_message_size)
			return WebSocketSendResult::TooLarge;

		if(!binary && size && !NaWServer::detail::validUtf8({reinterpret_cast<const char *>(bytes), size}))
			return WebSocketSendResult::InvalidArgument;

		NaWServer::detail::UniqueLock<NaWServer::detail::Mutex> lock(queue_mutex_, NaWServer::detail::tryToLock);
		if(!lock.owns_lock())
			return WebSocketSendResult::Busy;

		if(!accepting_.load() || !registry_.get(token_, Owner::WebSocket).valid)
			return WebSocketSendResult::Closed;

		if(registry_.transport_.supportsWriteLease())
		{
			const auto pressure = registry_.transport_.writePressure(registry_.get(token_, Owner::WebSocket));
			if(pressure.jobs >= limits_.max_pending_messages || pressure.bytes > limits_.max_pending_bytes ||
			    size > limits_.max_pending_bytes - pressure.bytes)
			{
				accepting_.store(false);
				overload_close_requested_.store(true);
				return WebSocketSendResult::QueueFull;
			}
			websocket::wire::FrameHeader header;
			if(!websocket::wire::makeFrameHeader(binary ? 2 : 1, size, header))
				return WebSocketSendResult::InvalidArgument;

			if(!policy.expires_at_ms)
				policy.expires_at_ms = deadlineAfter(registry_.transport_.transmissionMilliseconds(), send_timeout_);

			auto block = registry_.transport_.acquireWrite(registry_.get(token_, Owner::WebSocket), header.size + size, policy);
			if(!block) return WebSocketSendResult::GlobalFull;

			if(block.capacity() < header.size + size) return WebSocketSendResult::GlobalFull;

			std::memcpy(block.data(), header.bytes.data(), header.size);
			if(size) std::memcpy(block.data() + header.size, bytes, size);

			return block.commit(header.size + size) ? WebSocketSendResult::Queued : WebSocketSendResult::Closed;
		}
		if(policy.priority || policy.expires_at_ms || policy.discard_expired || policy.replacement_key)
			return WebSocketSendResult::InvalidArgument;

		if(pending_count_ == limits_.max_pending_messages ||
		    size > limits_.max_pending_bytes - pending_bytes_)
		{
			accepting_.store(false);
			overload_close_requested_.store(true);
			return WebSocketSendResult::QueueFull;
		}

		auto acquired = pool_.tryAcquire();
		switch(acquired.status)
		{
		case WebSocketTxPool::AcquireStatus::Busy:
			return WebSocketSendResult::Busy;
		case WebSocketTxPool::AcquireStatus::GlobalFull:
			return WebSocketSendResult::GlobalFull;
		case WebSocketTxPool::AcquireStatus::InvalidState:
			return WebSocketSendResult::Closed;
		case WebSocketTxPool::AcquireStatus::Acquired:
			break;
		}

		Pending pending;
		pending.block = std::move(acquired.block);
		if(size)
			std::memcpy(pending.block.data(), bytes, size);

		pending.size = size;
		pending.binary = binary;
		pending_[(queue_head_ + queued_) % limits_.max_pending_messages] = std::move(pending);
		++queued_;
		++pending_count_;
		pending_bytes_ += size;
		return WebSocketSendResult::Queued;
	}

	void WebSocketConnection::invalidate()
	{
		accepting_.store(false);
		aborted_.store(true);
	}

	void WebSocketConnection::abort()
	{
		invalidate();
	}

	bool WebSocketConnection::finished() const
	{
		return finished_.load() || aborted_.load();
	}

	bool WebSocketConnection::wantsRead() const
	{
		return opened_ && (!staged_input_ || close_drain_ == CloseDrain::Reading) &&
		       (!closing_ || close_drain_ == CloseDrain::Reading) &&
		       !overload_close_requested_.load() && !finished();
	}

	bool WebSocketConnection::wantsWrite(std::uint64_t now)
	{
		if(application_close_requested_.exchange(false, std::memory_order_acquire))
			requestClose();

		if(finished())
			return false;

		if(close_drain_ != CloseDrain::None)
		{
			if(now >= close_deadline_ || close_drained_ == close_drain_bytes_)
			{
				finished_.store(true);
				return false;
			}

			return false;
		}

		if(overload_close_requested_.exchange(false))
		{
			requestClose(1013);
			clearQueued();
		}

		if(!prepareOutput(now))
			return false;

		if(now >= output_deadline_)
		{
			abort();
			return false;
		}

		return true;
	}

	void WebSocketConnection::queueControl(std::uint8_t opcode, ByteView payload)
	{
		if(closing_ || finished())
			return;

		if(payload.size() > 125 || control_count_ == controls_.size())
		{
			requestClose(1011);
			return;
		}

		auto &control = controls_[(control_head_ + control_count_) % controls_.size()];
		control.opcode = opcode;
		control.size = payload.size();
		if(!payload.empty())
			std::memcpy(control.bytes.data(), payload.data(), payload.size());

		++control_count_;
	}

	void WebSocketConnection::requestClosePayload(ByteView payload)
	{
		if(closing_ || finished())
			return;

		accepting_.store(false);
		closing_ = true;
		// A close cancels decoder-held data before close-drain reuses Rx storage.
		// An already published callback owns a different buffer until it returns.
		staged_input_ = false;
		close_.opcode = 8;
		close_.size = payload.size();
		if(!payload.empty())
			std::memcpy(close_.bytes.data(), payload.data(), payload.size());
	}

	void WebSocketConnection::requestClose(std::uint16_t code)
	{
		if(!websocket::wire::validCloseCode(code))
		{
			abort();
			return;
		}

		const std::uint8_t payload[]{static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code)};
		requestClosePayload({payload, sizeof(payload)});
	}

	void WebSocketConnection::decoded(const websocket::wire::DecodeResult &result)
	{
		using websocket::wire::DecodeStatus;
		switch(result.status)
		{
		case DecodeStatus::NeedMore:
		case DecodeStatus::Pong:
			return;
		case DecodeStatus::Ping:
			queueControl(10, result.payload);
			return;
		case DecodeStatus::Close:
			requestClosePayload(result.payload);
			return;
		case DecodeStatus::Error:
			requestClose(result.close_code);
			return;
		case DecodeStatus::InvalidState:
			abort();
			return;
		case DecodeStatus::Message:
			break;
		}

		if(deferred_callbacks_)
		{
			// The decoder buffer remains untouched until this message has been
			// copied to the single-consumer mailbox. Full means TCP backpressure.
			staged_payload_ = result.payload;
			staged_binary_ = result.binary;
			staged_input_ = true;
			progressDeferredInput();
			return;
		}

		Exchange exchange(*this, false, nullptr, {WebSocketReadStatus::Message, result.payload, result.binary});
		Request request(exchange);
		Response response(exchange.responseTransport());
		handler_.handleRequest(&request, response);
	}

	void WebSocketConnection::consumePrefix(ByteView bytes)
	{
		std::size_t consumed = 0;
		if(!bytes.data() && bytes.size())
		{
			abort();
			return;
		}

		while(consumed < bytes.size() && !closing_ && wantsRead())
		{
			const auto result = decoder_.consume(bytes.data() + consumed, bytes.size() - consumed);
			if(result.consumed > bytes.size() - consumed ||
			    (!result.consumed && result.status == websocket::wire::DecodeStatus::NeedMore))
			{
				abort();
				return;
			}

			consumed += result.consumed;
			decoded(result);
		}
	}

	void WebSocketConnection::readReady(std::uint64_t now)
	{
		if(!wantsRead())
			return;

		if(close_drain_ == CloseDrain::Reading)
		{
			if(now >= close_deadline_)
			{
				finished_.store(true);
				return;
			}

			const auto handle = registry_.get(token_, Owner::WebSocket);
			if(!handle.valid)
			{
				abort();
				return;
			}

			// No parser/callback can use Rx after Close. Reuse it for raw discard;
			// one bounded syscall per readiness, never wait for a peer or allocate.
			const auto capacity = std::min({std::size_t{256}, limits_.max_message_size,
			                                close_drain_bytes_ - close_drained_});
			const auto result = registry_.transport_.readSome(handle, receive_storage_.get(), capacity);
			if(result.status == IoStatus::Progress && result.size > 0 && result.size <= capacity)
			{
				close_drained_ += result.size;
				if(close_drained_ == close_drain_bytes_)
					finished_.store(true);
			}
			else if(result.status == IoStatus::PeerClosed && !result.size)
				finished_.store(true);
			else if(result.size || (result.status != IoStatus::WouldBlock && result.status != IoStatus::Interrupted))
				abort();

			return;
		}

		const auto destination = decoder_.writable();
		const auto handle = registry_.get(token_, Owner::WebSocket);
		if(!handle.valid || !destination.data || !destination.size)
		{
			abort();
			return;
		}

		const auto result = registry_.transport_.readSome(handle, destination.data, destination.size);
		if(result.status == IoStatus::Progress && result.size > 0 && result.size <= destination.size)
		{
			decoded(decoder_.commit(result.size));
			return;
		}

		if(result.size || (result.status != IoStatus::WouldBlock && result.status != IoStatus::Interrupted))
			abort();
	}

	ByteView WebSocketConnection::outputPayload() const
	{
		if(output_ == Output::Data)
			return {active_data_.block.data(), active_data_.size};

		if(output_ == Output::Close)
			return {close_.bytes.data(), close_.size};

		if(output_ == Output::Control)
			return {controls_[control_head_].bytes.data(), controls_[control_head_].size};

		return {};
	}

	bool WebSocketConnection::prepareOutput(std::uint64_t now)
	{
		if(output_ != Output::None)
			return true;

		std::uint8_t opcode;
		if(closing_)
		{
			output_ = Output::Close;
			opcode = 8;
		}
		else if(control_count_)
		{
			output_ = Output::Control;
			opcode = controls_[control_head_].opcode;
		}
		else
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(queue_mutex_);
			if(!queued_)
				return false;

			active_data_ = std::move(pending_[queue_head_]);
			queue_head_ = (queue_head_ + 1) % limits_.max_pending_messages;
			--queued_;
			output_ = Output::Data;
			opcode = active_data_.binary ? 2 : 1;
		}

		if(!websocket::wire::makeFrameHeader(opcode, outputPayload().size(), output_header_))
		{
			abort();
			return false;
		}

		header_offset_ = 0;
		payload_offset_ = 0;
		output_deadline_ = deadlineAfter(now, send_timeout_);
		return true;
	}

	void WebSocketConnection::completeOutput(std::uint64_t now)
	{
		if(output_ == Output::Data)
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(queue_mutex_);
			pending_bytes_ -= active_data_.size;
			active_data_ = {};
			--pending_count_;
		}
		else if(output_ == Output::Control)
		{
			control_head_ = (control_head_ + 1) % controls_.size();
			--control_count_;
		}
		else if(output_ == Output::Close)
		{
			// Close was sent completely. Discard bounded trailing bytes before the
			// parent's one native close, without WS callbacks or a separate SHUT_WR.
			// The latter can outlive lwIP's TIME_WAIT PCB and stale its netconn.
			close_drain_ = CloseDrain::Reading;
			close_deadline_ = deadlineAfter(now, close_timeout_);
		}

		output_ = Output::None;
	}

	void WebSocketConnection::writeReady(std::uint64_t now)
	{
		if(!wantsWrite(now))
			return;

		const auto handle = registry_.get(token_, Owner::WebSocket);
		if(!handle.valid)
		{
			abort();
			return;
		}

		if(registry_.transport_.supportsWriteLease() && output_ != Output::Data)
		{
			const auto payload = outputPayload();
			auto block = registry_.transport_.acquireWrite(handle, output_header_.size + payload.size(),
			    {0, output_deadline_, false});
			if(!block) return;

			if(block.capacity() < output_header_.size + payload.size()) return;

			std::memcpy(block.data(), output_header_.bytes.data(), output_header_.size);
			if(!payload.empty()) std::memcpy(block.data() + output_header_.size, payload.data(), payload.size());

			if(!block.commit(output_header_.size + payload.size())) { abort(); return; }

			completeOutput(now);
			return;
		}

		const bool writing_header = header_offset_ < output_header_.size;
		const auto payload = outputPayload();
		const auto *bytes = writing_header ? output_header_.bytes.data() + header_offset_ : payload.data() +
		    payload_offset_;
		const auto size = writing_header ? output_header_.size - header_offset_ : payload.size() - payload_offset_;
		const auto result = registry_.transport_.writeSome(handle, bytes, size);
		if(result.status == IoStatus::Progress && result.size > 0 && result.size <= size)
		{
			if(writing_header)
				header_offset_ += result.size;
			else
				payload_offset_ += result.size;

			if(header_offset_ == output_header_.size && payload_offset_ == payload.size())
				completeOutput(now);

			return;
		}

		if(result.size || (result.status != IoStatus::WouldBlock && result.status != IoStatus::Interrupted))
			abort();
	}

	void WebSocketConnection::clearQueued()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(queue_mutex_);
		for(std::size_t index = 0; index < limits_.max_pending_messages; ++index)
		{
			if(pending_[index].block) pending_bytes_ -= pending_[index].size;

			pending_[index] = {};
		}

		pending_count_ -= queued_;
		queued_ = 0;
	}

	void WebSocketConnection::clearPending()
	{
		clearQueued();
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(queue_mutex_);
		active_data_ = {};
		pending_count_ = 0;
		pending_bytes_ = 0;
		output_ = Output::None;
		control_count_ = 0;
	}

	void WebSocketConnection::disconnect()
	{
		if(disconnected_)
			return;

		disconnected_ = true;
		invalidate();
		finished_.store(true);
		auto *client = std::exchange(client_, nullptr);
		if(client)
			WebSocketEndpoint::disconnect(client);

		clearPending();
		// External handles may retain this disconnected control object. They do
		// not retain the server's payload reservation or borrowed HTTP metadata.
		pending_.reset();
		header_entries_.reset();
		head_storage_.reset();
		receive_storage_.reset();
		callback_storage_.reset();
	}

	bool WebSocketConnection::prepareDeferredCallbacks()
	{
		return callback_storage_.allocate(pool_.memoryPool(), limits_.max_message_size);
	}

	void WebSocketConnection::enableDeferredCallbacks()
	{
		deferred_callbacks_ = true;
	}

	void WebSocketConnection::progressDeferredInput()
	{
		if(!staged_input_ || callback_ready_.load(std::memory_order_acquire)) return;

		if(!staged_payload_.empty())
			std::memcpy(callback_storage_.get(), staged_payload_.data(), staged_payload_.size());

		callback_size_ = staged_payload_.size();
		callback_binary_ = staged_binary_;
		staged_input_ = false;
		callback_ready_.store(true, std::memory_order_release);
	}

	void WebSocketConnection::dispatchDeferredCallback()
	{
		if(!callback_ready_.load(std::memory_order_acquire)) return;

		Exchange exchange(*this, false, nullptr, {WebSocketReadStatus::Message,
		    {callback_storage_.get(), callback_size_}, callback_binary_});
		Request request(exchange);
		Response response(exchange.responseTransport());
		handler_.handleRequest(&request, response);
		callback_ready_.store(false, std::memory_order_release);
	}

	void WebSocketConnection::finishIo()
	{
		invalidate();
		// The HTTP owner may now disconnect and free all arena storage before
		// closing the registry slot (which permits QoS to reclaim that arena).
		io_finished_.store(true, std::memory_order_release);
	}

	bool WebSocketConnection::ioFinished() const
	{
		return io_finished_.load(std::memory_order_acquire);
	}

} // namespace NaWServer::tcp::detail
