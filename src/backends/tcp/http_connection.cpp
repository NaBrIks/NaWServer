// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "http_connection.h"
#include "http/wire/response_writer.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace NaWServer::tcp::detail
{

	namespace
	{

		std::uint64_t addMilliseconds(std::uint64_t now, std::uint32_t interval)
		{
			const auto maximum = std::numeric_limits<std::uint64_t>::max();
			return interval > maximum - now ? maximum : now + interval;
		}

		class Exchange final : public HttpExchange
		{
			SessionIo &io_;
			http::wire::RequestParser &parser_;
			const std::uint8_t *prefix_;
			std::size_t prefix_size_;
			std::size_t body_offset_ = 0;
			std::optional<ByteView> prepared_body_;
			bool sent_ = false;
			bool stream_head_only_ = false;
			HttpConnectionBindings *bindings_;
			ResponseTransport override_{};
			HttpRequestState *state_ = nullptr;

			static SendResult sendResponse(void *context, int status, const char *type,
			    const Header *headers, std::size_t count, ByteView body)
			{
				return sendResponseImpl(context, status, type, headers, count, body, false);
			}

			static SendResult sendImmutable(void *context, int status, const char *type,
			    const Header *headers, std::size_t count, ByteView body)
			{
				return sendResponseImpl(context, status, type, headers, count, body, true);
			}

			static SendResult sendResponseImpl(void *context, int status, const char *type,
			    const Header *headers, std::size_t count, ByteView body, bool immutable)
			{
				auto &self = *static_cast<Exchange *>(context);
				if(self.sent_)
					return SendResult::AlreadySent;

				http::wire::ResponseWriter writer;
				const auto prepared = writer.prepare(self.parser_.head().method, status, type, headers, count, body);
				if(prepared != SendResult::Ok)
					return prepared;

				self.sent_ = true;
				while(!writer.complete())
				{
					const auto bytes = writer.current();
					const bool written = !bytes.empty() && (immutable && writer.bodySpan() ?
					    self.io_.writeImmutable(bytes.data(), bytes.size()) : self.io_.write(bytes.data(), bytes.size()));
					if(!written || !writer.advance(bytes.size()))
						return SendResult::TransportError;
				}
				return SendResult::Ok;
			}

			static SendResult beginStream(void *context, int status, const char *type,
			    const Header *headers, std::size_t count, std::size_t size)
			{
				auto &self = *static_cast<Exchange *>(context);
				if(self.sent_) return SendResult::AlreadySent;

				http::wire::ResponseWriter writer;
				const auto prepared = writer.prepareHead(self.parser_.head().method, status, type, headers, count, size);
				if(prepared != SendResult::Ok) return prepared;

				self.sent_ = true;
				self.stream_head_only_ = self.parser_.head().method == HttpMethod::Head;
				// Claim the direct wire path before any byte can escape. A read/write
				// failure must close it, never append a captured fallback response.
				if(self.override_.direct_started) self.override_.direct_started(self.override_.context);

				while(!writer.complete())
				{
					const auto bytes = writer.current();
					if(bytes.empty() || !self.io_.write(bytes.data(), bytes.size()) || !writer.advance(bytes.size()))
						return SendResult::TransportError;
				}
				return SendResult::Ok;
			}

			static SendResult writeStream(void *context, ByteView bytes)
			{
				auto &self = *static_cast<Exchange *>(context);
				return self.stream_head_only_ || self.io_.write(bytes.data(), bytes.size()) ?
				    SendResult::Ok : SendResult::TransportError;
			}

			static SendResult finishStream(void *context)
			{
				auto &self = *static_cast<Exchange *>(context);
				if(!self.io_.flush()) return SendResult::TransportError;

				return SendResult::Ok;
			}

			static WriteBuffer acquireStream(void *context, std::size_t capacity)
			{
				auto &self = *static_cast<Exchange *>(context);
				return self.stream_head_only_ ? WriteBuffer{} : self.io_.acquireWrite(capacity);
			}

			static SendResult sendSource(void *context, int status, const char *type,
			    const Header *headers, std::size_t count, ResponseSource source)
			{
				const auto prepared = beginStream(context, status, type, headers, count, source.size);
				if(prepared != SendResult::Ok) return prepared;

				auto &self = *static_cast<Exchange *>(context);
				if(!self.stream_head_only_ && !self.io_.writeSource(source)) return SendResult::TransportError;

				return finishStream(context);
			}

			static SendResult forwardSend(void *context, int status, const char *type,
			    const Header *headers, std::size_t count, ByteView body)
			{
				auto &self = *static_cast<Exchange *>(context);
				return self.override_.send(self.override_.context, status, type, headers, count, body);
			}

			static SendResult forwardImmutable(void *context, int status, const char *type,
			    const Header *headers, std::size_t count, ByteView body)
			{
				auto &self = *static_cast<Exchange *>(context);
				return self.override_.send_immutable(self.override_.context, status, type, headers, count, body);
			}

			static SendResult forwardOwned(void *context, int status, const char *type,
			    const Header *headers, std::size_t count, ByteView body, MemoryBlock &block)
			{
				auto &self = *static_cast<Exchange *>(context);
				return self.override_.send_owned(self.override_.context, status, type, headers, count, body, block);
			}

			static SendResult forwardSource(void *context, int status, const char *type,
			    const Header *headers, std::size_t count, OwnedResponseSource &source)
			{
				auto &self = *static_cast<Exchange *>(context);
				return self.override_.send_owned_source(self.override_.context, status, type, headers, count, source);
			}

		public:
			Exchange(SessionIo &io, http::wire::RequestParser &parser, const char *prefix, std::size_t size,
			    HttpConnectionBindings *bindings)
			  : io_(io), parser_(parser), prefix_(reinterpret_cast<const std::uint8_t *>(prefix)), prefix_size_(size),
			    bindings_(bindings)
			{
			}

			HttpRequestHead head() const override
			{
				return parser_.head();
			}

			void state(HttpRequestState &state) { state_ = &state; }
			HttpRequestState *requestState() const override { return state_; }
			std::optional<std::uint32_t> peerAddress() const override
			{
				const auto *peer = static_cast<const PeerAddress *>(extension(&peer_address_key));
				return peer && peer->valid ? std::optional<std::uint32_t>(peer->ipv4) : std::nullopt;
			}

			HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const override
			{
				return parser_.readHeader(name, destination, capacity);
			}

			BodyReadResult readBody(uint8_t *destination, std::size_t capacity) override
			{
				if(!destination || !capacity)
					return {BodyReadStatus::TransportError, 0};

				const auto length = head().content_length;
				if(body_offset_ == length)
					return {BodyReadStatus::End, 0};

				capacity = std::min(capacity, length - body_offset_);
				if(prepared_body_)
				{
					std::memcpy(destination, prepared_body_->data() + body_offset_, capacity);
					body_offset_ += capacity;
					return {BodyReadStatus::Data, capacity};
				}

				if(body_offset_ < prefix_size_)
				{
					const auto size = std::min(capacity, prefix_size_ - body_offset_);
					std::memcpy(destination, prefix_ + body_offset_, size);
					body_offset_ += size;
					return {BodyReadStatus::Data, size};
				}

				const auto result = io_.read(destination, capacity);
				if(result.status == BodyReadStatus::Data)
					body_offset_ += result.size;

				return result;
			}

			std::optional<ByteView> preparedBody() const override
			{
				return prepared_body_;
			}

			BodyReadStatus prepareBody(std::uint8_t *destination, std::size_t capacity)
			{
				const auto length = head().content_length;
				if(length > capacity)
					return BodyReadStatus::TransportError;

				const auto prefix = std::min(prefix_size_, length);
				if(prefix)
					std::memcpy(destination, prefix_, prefix);

				body_offset_ = prefix;
				while(body_offset_ < length)
				{
					const auto result = io_.read(destination + body_offset_, length - body_offset_);
					if(result.status != BodyReadStatus::Data)
						return result.status;

					body_offset_ += result.size;
				}
				prepared_body_ = ByteView{destination, length};
				body_offset_ = 0; // Preserve readBody for custom synchronous handlers.
				return BodyReadStatus::Data;
			}

			ResponseTransport responseTransport() override
			{
				ResponseTransport result{this, sendResponse};
				result.send_immutable = sendImmutable;
				if(override_.send) result.send = forwardSend;

				if(override_.send_immutable) result.send_immutable = forwardImmutable;

				if(override_.send_owned) result.send_owned = forwardOwned;
				if(override_.send_owned_source) result.send_owned_source = forwardSource;
				result.send_source = sendSource;
				result.begin_stream = beginStream;
				result.write_stream = writeStream;
				result.finish_stream = finishStream;
				result.acquire_stream = acquireStream;
				return result;
			}

			void prepared(ByteView body, ResponseTransport response)
			{
				prepared_body_ = body;
				override_ = response;
			}

			void *extension(const void *key) const override
			{
				return bindings_ ? bindings_->extension(key) : nullptr;
			}

			void drainSmallRejectedBody(const HttpLimits &limits)
			{
				const auto length = head().content_length;
				if(length <= limits.max_body_size || length - limits.max_body_size > 256)
					return;

				const auto deadline = addMilliseconds(io_.now(), 250);
				std::uint8_t discard[256];
				while(body_offset_ < length)
				{
					if(body_offset_ < prefix_size_)
					{
						body_offset_ = std::min(prefix_size_, length);
						continue;
					}

					const auto result = io_.readUntil(discard, std::min(sizeof(discard), length - body_offset_),
					        deadline);
					if(result.status != BodyReadStatus::Data)
						break;

					body_offset_ += result.size;
				}
			}

		};

		void sendRejection(Exchange &exchange, int status, const char *message = "HTTP request rejected")
		{
			Response response(exchange.responseTransport());
			(void)response.text(status, message);
		}

	} // namespace

	ConnectionDisposition executePreparedHttpConnection(SessionIo &io, http::wire::RequestParser &parser,
	    ByteView body, HttpRequestHandler &handler, HttpConnectionBindings *bindings, ResponseTransport response,
	    HttpRequestState &state)
	{
		Exchange exchange(io, parser, nullptr, 0, bindings);
		exchange.state(state);
		if(bindings) bindings->bindRequestState(&state);
		exchange.prepared({body.data(), std::min(body.size(), parser.head().content_length)}, response);
		if(bindings)
		{
			const auto result = bindings->tryUpgrade(exchange, parser, body, io);
			if(result == UpgradeDisposition::Http) return ConnectionDisposition::Http;

			if(result == UpgradeDisposition::Close) return ConnectionDisposition::Close;

			if(result == UpgradeDisposition::WebSocket) return ConnectionDisposition::WebSocket;
		}

		const auto disposition = handler.handle(exchange);
		if(disposition == ExchangeDisposition::Drop) return ConnectionDisposition::Drop;
		if(disposition == ExchangeDisposition::CloseConnection) return ConnectionDisposition::Close;
		return bindings && bindings->callbackComplete() ? ConnectionDisposition::External : ConnectionDisposition::Http;
	}

	ConnectionDisposition executeHttpConnection(SessionIo &io, HttpWorkspace workspace,
	    HttpRequestHandler &handler, const HttpLimits &limits, HttpConnectionBindings *bindings)
	{
		if(!workspace.body && workspace.body_capacity)
			return ConnectionDisposition::Close;

		// Preserve the common dispatcher's body-limit semantics and small-reject
		// drain. Parser validates framing before any application dispatch.
		auto wire_limits = limits;
		wire_limits.max_body_size = std::numeric_limits<std::size_t>::max();
		http::wire::RequestParser parser(workspace.head, workspace.head_capacity,
		    workspace.entries, workspace.entry_capacity, wire_limits);
		std::size_t used = 0;
		int rejection = 0;
		auto parsed = parser.advance(0);
		while(parsed.state == http::wire::ParseStatus::NeedMore)
		{
			if(used == limits.max_header_bytes)
			{
				rejection = 431;
				break;
			}

			const auto result = io.read(reinterpret_cast<std::uint8_t *>(workspace.head) + used,
			        limits.max_header_bytes - used);
			if(result.status != BodyReadStatus::Data)
			{
				rejection = result.status == BodyReadStatus::Timeout ? 408 : 400;
				break;
			}

			used += result.size;
			parsed = parser.advance(used);
		}
		if(parsed.state == http::wire::ParseStatus::Reject)
			rejection = parsed.status;

		if(parsed.state == http::wire::ParseStatus::InvalidState)
			return ConnectionDisposition::Close;

		const auto head_bytes = parsed.state == http::wire::ParseStatus::Complete ? parsed.head_bytes : used;
		Exchange exchange(io, parser, workspace.head + head_bytes, used - head_bytes, bindings);
		HttpRequestState state;
		exchange.state(state);
		struct ResourceHandoff
		{
			HttpConnectionBindings *bindings;
			HttpRequestState &state;
			~ResourceHandoff() { if(bindings) bindings->retainRequestResources(state); }
		} resource_handoff{bindings, state};
		if(bindings) bindings->bindRequestState(&state);
		if(rejection)
		{
			sendRejection(exchange, rejection);
			return ConnectionDisposition::Http;
		}
		const auto decision = handler.prepareHead(exchange, state);
		if(decision == HeaderDisposition::Drop) return ConnectionDisposition::Drop;
		if(decision == HeaderDisposition::Reply) return ConnectionDisposition::Close;
		if(state.body.mode == BodyMode::Stream)
		{
			// Runtime streaming uses the managed bounded state machine. Standalone
			// synchronous exchanges cannot silently buffer a large stream instead.
			sendRejection(exchange, 503, "Streaming requires managed connections");
			return ConnectionDisposition::Close;
		}

		if(bindings)
		{
			const ByteView prefix(reinterpret_cast<const std::uint8_t *>(workspace.head + head_bytes),
			    used - head_bytes);
			const auto upgrade = bindings->tryUpgrade(exchange, parser, prefix, io);
			if(upgrade == UpgradeDisposition::Http)
				return ConnectionDisposition::Http;

			if(upgrade == UpgradeDisposition::Close)
				return ConnectionDisposition::Close;

			if(upgrade == UpgradeDisposition::WebSocket)
				return ConnectionDisposition::WebSocket;
		}

		const auto head = exchange.head();
		if(workspace.body && head.content_length <= limits.max_body_size && !head.target.empty() &&
		    head.target.front() == '/' && head.target.size() <= limits.max_target_size)
		{
			const auto status = exchange.prepareBody(workspace.body, workspace.body_capacity);
			if(status != BodyReadStatus::Data)
			{
				sendRejection(exchange, status == BodyReadStatus::Timeout ? 408 : 400, "Incomplete request body");
				return ConnectionDisposition::Http;
			}
		}

		const auto disposition = handler.handle(exchange);
		if(disposition == ExchangeDisposition::Drop) return ConnectionDisposition::Drop;
		if(disposition == ExchangeDisposition::CloseConnection) return ConnectionDisposition::Close;
		return bindings && bindings->callbackComplete() ? ConnectionDisposition::External : ConnectionDisposition::Http;
	}

} // namespace NaWServer::tcp::detail
