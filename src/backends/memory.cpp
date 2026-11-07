// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/memory.h>
#include <NaWServer/backends/memory_sse.h>
#include <NaWServer/websocket.h>
#include <NaWServer/request.h>
#include "detail/ascii.h"
#include <algorithm>
#include <cstring>
#include <new>

namespace NaWServer
{
	namespace
	{
		class MemoryExchange final : public HttpExchange, public sse::Exchange
		{
			const MemoryRequest &request_;
			MemoryResponse &response_;
			std::size_t offset_ = 0;
			void *extension_ = nullptr;
			MemoryHttpBackend *backend_ = nullptr;
			std::size_t sse_capture_capacity_;
			static SendResult send(void *context, int status, const char *type,
			    const Header *headers, std::size_t count, ByteView body)
			{
				auto &self = *static_cast<MemoryExchange *>(context);
				++self.response_.send_calls;
				if(self.request_.fail_send) return SendResult::TransportError;

				self.response_.status = status;
				self.response_.content_type = type ? type : "";
				for(std::size_t index = 0; index < count; ++index)
					self.response_.headers.emplace_back(headers[index].name, headers[index].value);
				if(!body.empty()) self.response_.body.assign(body.data(), body.data() + body.size());

				return SendResult::Ok;
			}

		public:
			MemoryExchange(const MemoryRequest &request, MemoryResponse &response, void *extension = nullptr,
			    MemoryHttpBackend *backend = nullptr, std::size_t sse_capture_capacity = 16 * 1024)
			  : request_(request), response_(response), extension_(extension), backend_(backend),
			    sse_capture_capacity_(sse_capture_capacity)
			{
			}

			HttpRequestHead head() const override
			{
				return {request_.method, request_.target, request_.content_length};
			}

			HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const override
			{
				if(!name || !*name || (!destination && capacity)) return {HeaderReadStatus::InvalidArgument, 0};

				for(const auto &header : request_.headers)
				{
					if(!detail::asciiEqualIgnoreCase(header.first, name)) continue;

					if(capacity <= header.second.size())
						return {HeaderReadStatus::BufferTooSmall, header.second.size()};

					std::memcpy(destination, header.second.c_str(), header.second.size() + 1);
					return {HeaderReadStatus::Ok, header.second.size()};
				}
				return {HeaderReadStatus::NotFound, 0};
			}

			BodyReadResult readBody(uint8_t *destination, std::size_t capacity) override
			{
				++response_.read_calls;
				if(offset_ >= request_.fail_read_after) return {request_.read_failure, 0};

				const auto size = std::min({capacity, request_.read_chunk_size, request_.body.size() - offset_,
				                            request_.fail_read_after - offset_});
				if(!size) return {BodyReadStatus::End, 0};

				std::memcpy(destination, request_.body.data() + offset_, size);
				offset_ += size;
				return {BodyReadStatus::Data, size};
			}

			ResponseTransport responseTransport() override
			{
				return {this, send};
			}

			void *extension(const void *key) const override
			{
				if(key == &websocket_exchange_key) return extension_;

				if(key == &sse::exchange_key)
					return const_cast<sse::Exchange *>(static_cast<const sse::Exchange *>(this));

				return nullptr;
			}

			const void *owner() const override
			{
				return backend_;
			}

			std::shared_ptr<sse::Stream> retain() override
			{
				if(!backend_) return {};

				try
				{
					response_.sse_stream = std::make_shared<MemorySseStream>(sse_capture_capacity_);
				}
				catch(const std::bad_alloc &) { return {}; }
				return response_.sse_stream;
			}

		};
		MemoryRequest websocketRequest(const std::string &path)
		{
			MemoryRequest request;
			request.target = path;
			return request;
		}

	}
	struct MemoryHttpBackend::WebSocketSlot
	{
		std::uint64_t generation = 0;
		bool reserved = false;
		WebSocketClient *client = nullptr;
		std::shared_ptr<WebSocketSession> session;
		std::vector<MemoryWebSocketMessage> pending;
		std::string path;
		unsigned max_pending_messages = 0;
		std::size_t max_message_size = 0;
		std::size_t max_pending_bytes = 0;
	};
	class MemoryHttpBackend::MemorySession final : public WebSocketSession
	{
		MemoryHttpBackend *backend_;
		std::size_t slot_;
		std::uint64_t generation_;
		bool valid_ = true;
	public:
		MemorySession(MemoryHttpBackend *backend, std::size_t slot, std::uint64_t generation)
		  : backend_(backend), slot_(slot), generation_(generation)
		{
		}

		WebSocketSendResult sendCopy(const uint8_t *data, std::size_t size, bool binary) override
		{
			if(!valid_) return WebSocketSendResult::Closed;

			return backend_->queueWebSocket(slot_, generation_, data, size, binary);
		}

		void invalidate() override
		{
			valid_ = false;
		}

	};
	class MemoryWebSocketExchange final : public WebSocketExchange
	{
		MemoryHttpBackend &backend_;
		std::size_t slot_;
		std::uint64_t generation_;
		bool opening_;
		const uint8_t *data_ = nullptr;
		std::size_t size_ = 0;
		bool binary_ = false;
		bool close_requested_ = false;
	public:
		MemoryWebSocketExchange(MemoryHttpBackend &backend, std::size_t slot,
		    std::uint64_t generation, bool opening, const uint8_t *data = nullptr,
		    std::size_t size = 0, bool binary = false)
		  : backend_(backend), slot_(slot), generation_(generation), opening_(opening),
		    data_(data), size_(size), binary_(binary)
		{
		}

		bool opening() const override
		{
			return opening_;
		}

		std::shared_ptr<WebSocketSession> createSession(const WebSocketLimits &) override
		{
			std::shared_ptr<WebSocketSession> session;
			try
			{
				session = std::make_shared<MemoryHttpBackend::MemorySession>(
					&backend_, slot_, generation_);
			}
			catch(const std::bad_alloc &) { return {}; }
			if(session && slot_ < backend_.websocket_slots_.size())
				backend_.websocket_slots_[slot_]->session = session;

			return session;
		}

		WebSocketClient *client() const override;
		void attach(WebSocketClient *client) override;
		WebSocketReadResult readMessage(const WebSocketLimits &limits) override
		{
			if(size_ > limits.max_message_size || (!data_ && size_))
				return {WebSocketReadStatus::Error, {}, false};

			return {WebSocketReadStatus::Message, {data_, size_}, binary_};
		}

		void close() override
		{
			close_requested_ = true;
		}

		bool closeRequested() const
		{
			return close_requested_;
		}

	};
	WebSocketClient *MemoryWebSocketExchange::client() const
	{
		if(slot_ >= backend_.websocket_slots_.size()) return nullptr;

		const auto &slot = *backend_.websocket_slots_[slot_];
		return slot.generation == generation_ ? slot.client : nullptr;
	}

	void MemoryWebSocketExchange::attach(WebSocketClient *client)
	{
		if(slot_ >= backend_.websocket_slots_.size()) return;

		auto &slot = *backend_.websocket_slots_[slot_];
		if(slot.generation == generation_) slot.client = client;
	}

	HttpError MemoryHttpBackend::start(HttpRequestHandler &handler, const HttpLimits &limits)
	{
		if(handler_) return HttpError::InvalidState;

		if(!limits.max_body_size || !limits.max_header_bytes || !limits.max_headers ||
		    !limits.max_target_size || !limits.max_connections || !limits.exchange_timeout_ms)
			return HttpError::InvalidArgument;

		const auto routes = handler.transportRoutes();
		for(std::size_t index = 0; index < routes.count; ++index)
		{
			if(!routes.data || !routes.data[index]) return HttpError::InvalidArgument;

			const char *path = routes.data[index]->websocket_uri();
			if(!path) continue;

			const std::string_view uri(path);
			if(uri.empty() || uri.front() != '/' || uri.find('?') != std::string_view::npos ||
			    uri.size() > limits.max_target_size)
				return HttpError::InvalidArgument;

			for(std::size_t other = 0; other < index; ++other)
				if(routes.data[other] && routes.data[other]->websocket_uri() &&
				    uri == routes.data[other]->websocket_uri()) return HttpError::InvalidArgument;
		}

		handler_ = &handler;
		limits_ = limits;
		return HttpError::Ok;
	}

	HttpError MemoryHttpBackend::stop()
	{
		if(handling_) return HttpError::InvalidState;

		for(std::size_t index = 0; index < websocket_slots_.size(); ++index)
		{
			auto &slot = *websocket_slots_[index];
			if(slot.client) WebSocketEndpoint::disconnect(slot.client);

			slot.client = nullptr;
			slot.session.reset();
			slot.pending.clear();
			slot.reserved = false;
			// Keep the generation monotonic across stop/start. A stale handle can
			// never become valid when a slot is reused.
			slot.generation = 0;
		}

		handler_ = nullptr;
		return HttpError::Ok;
	}

	MemoryResponse MemoryHttpBackend::perform(const MemoryRequest &request)
	{
		MemoryResponse response;
		if(!handler_ || handling_) return response;

		std::size_t bytes = 0;
		for(const auto &header : request.headers)
		{
			if(header.first.size() > limits_.max_header_bytes - bytes ||
			    header.second.size() > limits_.max_header_bytes - bytes - header.first.size())
			{
				response.result = HttpError::InvalidArgument;
				return response;
			}

			bytes += header.first.size() + header.second.size();
		}
		if(request.headers.size() > limits_.max_headers)
		{
			response.result = HttpError::InvalidArgument;
			return response;
		}

		MemoryExchange exchange(request, response, nullptr, this, sse_capture_capacity_);
		handling_ = true;
		struct Reset
		{
			bool &flag;
			~Reset()
			{
				flag = false;
			}

		} reset{handling_};
		response.disposition = handler_->handle(exchange);
		response.result = HttpError::Ok;
		return response;
	}

	MemoryHttpBackend::MemoryHttpBackend(std::size_t sse_capture_capacity)
	  : sse_capture_capacity_(sse_capture_capacity)
	{
	}

	MemoryHttpBackend::~MemoryHttpBackend()
	{
		(void)stop();
	}

	MemoryWebSocketHandle MemoryHttpBackend::connectWebSocket(const std::string &path)
	{
		if(!handler_ || handling_ || path.empty() || path.front() != '/' || path.find('?') != std::string::npos)
			return {};

		const auto routes = handler_->transportRoutes();
		WebSocketEndpoint *endpoint = nullptr;
		for(std::size_t index = 0; index < routes.count; ++index)
		{
			if(!routes.data || !routes.data[index]) continue;

			const char *uri = routes.data[index]->websocket_uri();
			if(uri && path == uri) { endpoint = dynamic_cast<WebSocketEndpoint *>(routes.data[index]); break; }
		}
		if(!endpoint) return {};

		std::size_t slot_index = 0;
		for(; slot_index < websocket_slots_.size(); ++slot_index)
			if(!websocket_slots_[slot_index]->client && !websocket_slots_[slot_index]->reserved) break;

		if(slot_index == websocket_slots_.size())
		{
			try { websocket_slots_.emplace_back(new WebSocketSlot); }
			catch(const std::bad_alloc &) { return {}; }
		}

		auto &slot = *websocket_slots_[slot_index];
		if(!next_generation_) return {};

		try { slot.path = path; }
		catch(const std::bad_alloc &) { return {}; }
		slot.reserved = true;
		slot.generation = next_generation_;
		if(next_generation_ == std::numeric_limits<std::uint64_t>::max()) next_generation_ = 0;
		else ++next_generation_;

		slot.max_pending_messages = endpoint->limits().max_pending_messages;
		slot.max_message_size = endpoint->limits().max_message_size;
		slot.max_pending_bytes = endpoint->limits().max_pending_bytes;
		const auto generation = slot.generation;
		MemoryWebSocketExchange websocket_exchange(*this, slot_index, generation, true);
		auto request = websocketRequest(path);
		MemoryResponse response;
		MemoryExchange exchange(request, response, &websocket_exchange);
		handling_ = true;
		struct Reset { bool &flag; ~Reset()
					   {
						   flag = false;
					   }

		} reset{handling_};
		(void)handler_->handle(exchange);
		slot.reserved = false;
		if(websocket_exchange.closeRequested() && slot.client)
			(void)stopWebSocket({slot_index, generation});

		if(slot.generation != generation || !slot.client) return {};

		return {slot_index, generation};
	}

	WebSocketReadStatus MemoryHttpBackend::receive(MemoryWebSocketHandle handle, const uint8_t *data,
	    std::size_t size, bool binary)
	{
		if(handle.slot >= websocket_slots_.size()) return WebSocketReadStatus::Error;

		auto &slot = *websocket_slots_[handle.slot];
		if(!slot.client || slot.generation != handle.generation) return WebSocketReadStatus::Closed;

		if(!handler_ || handling_) return WebSocketReadStatus::Error;

		MemoryWebSocketExchange websocket_exchange(*this, handle.slot, handle.generation, false,
		    data, size, binary);
		auto request = websocketRequest(slot.path);
		MemoryResponse response;
		MemoryExchange exchange(request, response, &websocket_exchange);
		handling_ = true;
		struct Reset { bool &flag; ~Reset()
					   {
						   flag = false;
					   }

		} reset{handling_};
		(void)handler_->handle(exchange);
		if(websocket_exchange.closeRequested())
		{
			(void)stopWebSocket(handle);
			return WebSocketReadStatus::Closed;
		}

		return WebSocketReadStatus::Message;
	}

	std::vector<MemoryWebSocketMessage> MemoryHttpBackend::takeSent(MemoryWebSocketHandle handle)
	{
		if(handle.slot >= websocket_slots_.size()) return {};

		auto &slot = *websocket_slots_[handle.slot];
		if(!slot.client || slot.generation != handle.generation) return {};

		std::vector<MemoryWebSocketMessage> result;
		result.swap(slot.pending);
		return result;
	}

	bool MemoryHttpBackend::close(MemoryWebSocketHandle handle)
	{
		if(handling_) return false;

		return stopWebSocket(handle);
	}

	bool MemoryHttpBackend::stopWebSocket(MemoryWebSocketHandle handle)
	{
		if(handle.slot >= websocket_slots_.size()) return false;

		auto &slot = *websocket_slots_[handle.slot];
		if(!slot.client || slot.generation != handle.generation) return false;

		auto *client = slot.client;
		slot.client = nullptr;
		WebSocketEndpoint::disconnect(client);
		slot.pending.clear();
		slot.session.reset();
		slot.generation = 0;
		return true;
	}

	std::size_t MemoryHttpBackend::websocketCount() const
	{
		std::size_t count = 0;
		for(const auto &slot : websocket_slots_)
			if(slot->client) ++count;

		return count;
	}

	std::size_t MemoryHttpBackend::pendingMessageCount(MemoryWebSocketHandle handle) const
	{
		if(handle.slot >= websocket_slots_.size()) return 0;

		const auto &slot = *websocket_slots_[handle.slot];
		return slot.client && slot.generation == handle.generation ? slot.pending.size() : 0;
	}

	WebSocketSendResult MemoryHttpBackend::queueWebSocket(std::size_t index, std::uint64_t generation,
	    const uint8_t *data, std::size_t size, bool binary)
	{
		if(index >= websocket_slots_.size()) return WebSocketSendResult::Closed;

		auto &slot = *websocket_slots_[index];
		if(!slot.client || slot.generation != generation) return WebSocketSendResult::Closed;

		// Endpoint-level validation occurs before the transport call. The memory
		// transport enforces its configured per-session queue bound here.
		if(!data && size) return WebSocketSendResult::InvalidArgument;

		if(size > slot.max_message_size) return WebSocketSendResult::TooLarge;

		if(slot.pending.size() >= slot.max_pending_messages) return WebSocketSendResult::QueueFull;

		std::size_t pending_bytes = 0;
		for(const auto &message : slot.pending)
			pending_bytes += message.bytes.size();
		if(size > slot.max_pending_bytes - pending_bytes) return WebSocketSendResult::QueueFull;

		try
		{
			MemoryWebSocketMessage message;
			if(size) message.bytes.assign(data, data + size);

			message.binary = binary;
			slot.pending.push_back(std::move(message));
		}
		catch(const std::bad_alloc &) { return WebSocketSendResult::NoMemory; }
		return WebSocketSendResult::Queued;
	}

}
