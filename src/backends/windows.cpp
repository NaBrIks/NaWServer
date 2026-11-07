// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#include <NaWServer/backends/windows.h>
#include <NaWServer/request.h>
#include <NaWServer/websocket.h>
#include "detail/ascii.h"
#include "http/header_validation.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NaWServer
{
	namespace detail
	{
		class WindowsSseStream final : public sse::Stream
		{
			WindowsHttpBackend &backend_;
			SOCKET socket_;
			mutable std::mutex mutex_;
			bool committed_ = false;
			bool callback_active_ = true;
			bool finish_requested_ = false;
			bool closed_ = false;
			bool registered_ = false;

			void releaseLocked()
			{
				if(!registered_) return;

				registered_ = false;
				backend_.releaseSseStream(this);
			}

			void finishIfReadyLocked()
			{
				if(!finish_requested_ || callback_active_ || closed_) return;

				if(socket_ != INVALID_SOCKET)
				{
					closesocket(socket_);
					socket_ = INVALID_SOCKET;
				}

				closed_ = true;
				releaseLocked();
			}

		public:
			WindowsSseStream(WindowsHttpBackend &backend, SOCKET socket)
			  : backend_(backend), socket_(socket)
			{
			}

			void markRegistered()
			{
				registered_ = true;
			}

			void activate() override
			{
				std::lock_guard<std::mutex> lock(mutex_);
				committed_ = true;
			}

			void rollback() override
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if(committed_) return;

				closed_ = true;
				releaseLocked();
			}

			int write(const char *bytes, std::size_t size) override
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if(!committed_ || closed_ || finish_requested_ || (!bytes && size)) return -1;

				if(!size) return 0;

				const int written = ::send(socket_, bytes,
				        static_cast<int>(std::min(size, static_cast<std::size_t>(INT_MAX))), 0);
				if(written == SOCKET_ERROR)
					return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;

				return written;
			}

			void finish() override
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if(closed_ || finish_requested_) return;

				finish_requested_ = true;
				if(socket_ != INVALID_SOCKET) (void)shutdown(socket_, SD_SEND);

				finishIfReadyLocked();
			}

			bool isOpen() const override
			{
				std::lock_guard<std::mutex> lock(mutex_);
				return committed_ && !closed_ && socket_ != INVALID_SOCKET;
			}

			void callbackComplete()
			{
				std::lock_guard<std::mutex> lock(mutex_);
				callback_active_ = false;
				finishIfReadyLocked();
			}

			static std::shared_ptr<WindowsSseStream> create(WindowsHttpBackend &backend, SOCKET socket)
			{
				try
				{
					auto stream = std::make_shared<WindowsSseStream>(backend, socket);
					if(!backend.registerSseStream(stream)) return {};

					stream->markRegistered();
					return stream;
				}
				catch(const std::bad_alloc &) { return {}; }
			}

		};

		class WindowsSseExchange final : public sse::Exchange
		{
			WindowsHttpBackend &backend_;
			SOCKET socket_;
			std::shared_ptr<WindowsSseStream> stream_;

		public:
			WindowsSseExchange(WindowsHttpBackend &backend, SOCKET socket)
			  : backend_(backend), socket_(socket)
			{
			}

			const void *owner() const override
			{
				return &backend_;
			}

			std::shared_ptr<sse::Stream> retain() override
			{
				if(stream_) return {};

				stream_ = WindowsSseStream::create(backend_, socket_);
				return stream_;
			}

			bool active() const
			{
				return stream_ && stream_->isOpen();
			}

			void rollback()
			{
				if(stream_ && !stream_->isOpen()) stream_->rollback();
			}

			void callbackComplete()
			{
				if(stream_ && stream_->isOpen()) stream_->callbackComplete();
			}

		};
	}

	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr auto invalid_socket = static_cast<std::uintptr_t>(INVALID_SOCKET);
		constexpr char websocket_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
		bool hasToken(std::string_view values, std::string_view token)
		{
			while(!values.empty())
			{
				const auto comma = values.find(',');
				auto value = values.substr(0, comma);
				while(!value.empty() && (value.front() == ' ' || value.front() == '\t'))
					value.remove_prefix(1);
				while(!value.empty() && (value.back() == ' ' || value.back() == '\t'))
					value.remove_suffix(1);
				if(detail::asciiEqualIgnoreCase(value, token)) return true;

				if(comma == values.npos) break;

				values.remove_prefix(comma + 1);
			}
			return false;
		}

		bool validWebSocketKey(std::string_view key)
		{
			if(key.size() != 24 || key.substr(22) != "==") return false;

			std::array<BYTE, 32> decoded{};
			DWORD decoded_size = static_cast<DWORD>(decoded.size());
			if(!CryptStringToBinaryA(key.data(), static_cast<DWORD>(key.size()), CRYPT_STRING_BASE64,
			    decoded.data(), &decoded_size, nullptr, nullptr) || decoded_size != 16) return false;

			std::array<char, 32> encoded{};
			DWORD encoded_size = static_cast<DWORD>(encoded.size());
			const bool encoded_ok = CryptBinaryToStringA(decoded.data(), decoded_size,
			        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded.data(), &encoded_size) != FALSE;
			return encoded_ok && encoded_size == key.size() &&
			       std::string_view(encoded.data(), key.size()) == key;
		}

		bool websocketAccept(std::string_view key, std::string &accept)
		{
			const std::string source = std::string(key) + websocket_guid;
			HCRYPTPROV provider = 0;
			HCRYPTHASH hash = 0;
			std::array<BYTE, 20> digest{};
			DWORD digest_size = static_cast<DWORD>(digest.size());
			const bool ok = CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) &&
			    CryptCreateHash(provider, CALG_SHA1, 0, 0, &hash) &&
			    CryptHashData(hash, reinterpret_cast<const BYTE *>(source.data()), static_cast<DWORD>(source.size()),
			        0) &&
			    CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digest_size, 0);
			if(hash) CryptDestroyHash(hash);

			if(provider) CryptReleaseContext(provider, 0);

			if(!ok || digest_size != digest.size()) return false;

			std::array<char, 64> encoded{};
			DWORD encoded_size = static_cast<DWORD>(encoded.size());
			if(!CryptBinaryToStringA(digest.data(), digest_size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
			    encoded.data(), &encoded_size)) return false;

			accept.assign(encoded.data(), encoded_size);
			return true;
		}

		// FIN lets the peer consume the response before close. Discard only a bounded
		// remainder so rejected oversized headers do not turn that response into RST.
		void finishConnection(SOCKET socket, const std::atomic<bool> &stopping)
		{
			(void)shutdown(socket, SD_SEND);
			const auto deadline = Clock::now() + std::chrono::milliseconds(50);
			std::size_t budget = 4096;
			char discard[256];
			while(budget && !stopping && Clock::now() < deadline)
			{
				const auto received = recv(socket, discard, static_cast<int>(std::min(budget, sizeof(discard))), 0);
				if(received > 0) { budget -= static_cast<std::size_t>(received); continue; }

				if(received == 0 || WSAGetLastError() != WSAEWOULDBLOCK) break;

				fd_set readable;
				FD_ZERO(&readable);
				FD_SET(socket, &readable);
				const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline -
				        Clock::now()).count();
				if(remaining <= 0) break;

				timeval timeout{0, static_cast<long>(remaining)};
				if(select(0, &readable, nullptr, nullptr, &timeout) <= 0) break;
			}
		}

		class SocketExchange final : public HttpExchange
		{
			SOCKET socket_;
			const HttpLimits &limits_;
			const std::atomic<bool> &stopping_;
			Clock::time_point deadline_;
			HttpMethod method_ = HttpMethod::Unknown;
			bool http11_ = false;
			std::string target_, pending_body_;
			std::size_t content_length_ = 0, body_offset_ = 0;
			std::vector<std::pair<std::string, std::string> > headers_;
			WebSocketExchange *websocket_extension_ = nullptr;
			detail::WindowsSseExchange sse_exchange_;
			bool sent_ = false;
			bool wait(bool writing)
			{
				while(!stopping_ && Clock::now() < deadline_)
				{
					fd_set ready;
					FD_ZERO(&ready);
					FD_SET(socket_, &ready);
					const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline_ -
					        Clock::now()).count();
					const auto step = std::max<std::int64_t>(1, std::min<std::int64_t>(remaining, 100000));
					timeval timeout{0, static_cast<long>(step)};
					const auto result = select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, nullptr,
					        &timeout);
					if(result > 0) return true;

					if(result == SOCKET_ERROR) return false;
				}
				return false;
			}

			bool writeAll(const char *data, std::size_t size)
			{
				while(size)
				{
					if(!wait(true)) return false;

					const auto written = ::send(socket_, data,
					        static_cast<int>(std::min(size, static_cast<std::size_t>(INT_MAX))), 0);
					if(written == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;

					if(written <= 0) return false;

					data += written;
					size -= static_cast<std::size_t>(written);
				}
				return true;
			}

			static SendResult sendResponse(void *context, int status, const char *type,
			    const Header *headers, std::size_t count, ByteView body)
			{
				auto &self = *static_cast<SocketExchange *>(context);
				if(self.sent_) return SendResult::AlreadySent;

				self.sent_ = true;
				const bool no_body = status == 204 || status == 304 || status < 200;
				std::string wire = "HTTP/1.1 " + std::to_string(status) + " Response\r\nConnection: close\r\n";
				if(!no_body) wire += "Content-Length: " + std::to_string(body.size()) + "\r\n";

				if(type && *type) wire += std::string("Content-Type: ") + type + "\r\n";

				for(std::size_t index = 0; index < count; ++index)
				{
					if(detail::asciiEqualIgnoreCase(headers[index].name, "Connection")) continue;

					wire += std::string(headers[index].name) + ": " + headers[index].value + "\r\n";
				}
				wire += "\r\n";
				if(!self.writeAll(wire.data(), wire.size())) return SendResult::TransportError;

				if(!no_body && self.method_ != HttpMethod::Head && !body.empty() &&
				    !self.writeAll(reinterpret_cast<const char *>(body.data()), body.size()))
					return SendResult::TransportError;

				return SendResult::Ok;
			}

		public:
			SocketExchange(SOCKET socket, const HttpLimits &limits, const std::atomic<bool> &stopping,
			    WindowsHttpBackend &backend)
			  : socket_(socket), limits_(limits), stopping_(stopping),
			    deadline_(Clock::now() + std::chrono::milliseconds(limits.exchange_timeout_ms)),
			    sse_exchange_(backend, socket)
			{
			}

			// Returns zero on success or a response status before application dispatch.
			int receiveHead()
			{
				std::string buffer;
				std::size_t end = std::string::npos;
				while((end = buffer.find("\r\n\r\n")) == std::string::npos)
				{
					if(buffer.size() >= limits_.max_header_bytes) return 431;

					if(!wait(false)) return 408;

					char chunk[1024];
					const auto received = recv(socket_, chunk,
					        static_cast<int>(std::min(sizeof(chunk), limits_.max_header_bytes - buffer.size())), 0);
					if(received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;

					if(received <= 0) return 400;

					buffer.append(chunk, static_cast<std::size_t>(received));
				}
				pending_body_ = buffer.substr(end + 4);
				const auto first_end = buffer.find("\r\n");
				const std::string_view line(buffer.data(), first_end);
				const auto space = line.find(' '), second_space = line.find(' ', space == line.npos ? 0 : space + 1);
				if(space == line.npos || second_space == line.npos || line.find(' ', second_space + 1) != line.npos)
					return 400;

				const auto verb = line.substr(0, space), version = line.substr(second_space + 1);
				if(version != "HTTP/1.1" && version != "HTTP/1.0") return 505;

				http11_ = version == "HTTP/1.1";

				for(const auto &entry : {std::pair<std::string_view, HttpMethod>{"GET", HttpMethod::Get},
										 {"POST", HttpMethod::Post}, {"PUT", HttpMethod::Put},
										 {"DELETE", HttpMethod::Delete},
										 {"PATCH", HttpMethod::Patch}, {"OPTIONS", HttpMethod::Options},
										 {"HEAD", HttpMethod::Head}})
					if(verb == entry.first) method_ = entry.second;

				if(method_ == HttpMethod::Unknown) return 501;

				target_ = line.substr(space + 1, second_space - space - 1);
				if(target_.empty() || target_[0] != '/' || target_.find('#') != target_.npos) return 400;

				if(target_.size() > limits_.max_target_size) return 414;

				for(unsigned char character : target_)
					if(character <= 32 || character == 127) return 400;

				bool length_seen = false, host_seen = false, transfer_seen = false, expect_seen = false;
				std::size_t cursor = first_end + 2;
				while(cursor < end)
				{
					const auto next = buffer.find("\r\n", cursor);
					const std::string_view field(buffer.data() + cursor, next - cursor);
					cursor = next + 2;
					const auto colon = field.find(':');
					if(colon == field.npos) return 400;

					const auto name = field.substr(0, colon);
					auto value = field.substr(colon + 1);
					while(!value.empty() && (value.front() == ' ' || value.front() == '\t'))
						value.remove_prefix(1);
					while(!value.empty() && (value.back() == ' ' || value.back() == '\t'))
						value.remove_suffix(1);
					if(!detail::isValidHeaderName(name) || !detail::isValidHeaderValue(value)) return 400;

					if(headers_.size() >= limits_.max_headers) return 431;

					if(detail::asciiEqualIgnoreCase(name, "Content-Length"))
					{
						if(length_seen || value.empty()) return 400;

						length_seen = true;
						const auto result = std::from_chars(value.data(), value.data() + value.size(), content_length_);
						if(result.ec != std::errc{} || result.ptr != value.data() + value.size()) return 400;
					}

					if(detail::asciiEqualIgnoreCase(name, "Host"))
					{
						if(host_seen || value.empty()) return 400;

						host_seen = true;
					}

					if(detail::asciiEqualIgnoreCase(name, "Transfer-Encoding")) transfer_seen = true;

					if(detail::asciiEqualIgnoreCase(name, "Expect")) expect_seen = true;

					headers_.emplace_back(name, value);
				}
				if(version == "HTTP/1.1" && !host_seen) return 400;

				if(transfer_seen) return length_seen ? 400 : 501;

				if(expect_seen) return 417;

				return 0;
			}

			HttpRequestHead head() const override
			{
				return {method_, target_, content_length_};
			}

			void drainSmallRejectedBody()
			{
				if(content_length_ <= limits_.max_body_size || content_length_ - limits_.max_body_size > 256) return;

				const auto previous_deadline = deadline_;
				deadline_ = std::min(deadline_, Clock::now() + std::chrono::milliseconds(250));
				std::size_t remaining = content_length_;
				uint8_t discard[256];
				while(remaining)
				{
					const auto result = readBody(discard, std::min(remaining, sizeof(discard)));
					if(result.status != BodyReadStatus::Data || !result.size) break;

					remaining -= result.size;
				}
				deadline_ = previous_deadline;
			}

			HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const override
			{
				if(!name || !*name || (!destination && capacity)) return {HeaderReadStatus::InvalidArgument, 0};

				for(const auto &header : headers_)
				{
					if(!detail::asciiEqualIgnoreCase(name, header.first)) continue;

					if(capacity <= header.second.size())
						return {HeaderReadStatus::BufferTooSmall, header.second.size()};

					std::memcpy(destination, header.second.c_str(), header.second.size() + 1);
					return {HeaderReadStatus::Ok, header.second.size()};
				}
				return {HeaderReadStatus::NotFound, 0};
			}

			BodyReadResult readBody(uint8_t *destination, std::size_t capacity) override
			{
				if(body_offset_ < pending_body_.size())
				{
					const auto size = std::min(capacity, pending_body_.size() - body_offset_);
					std::memcpy(destination, pending_body_.data() + body_offset_, size);
					body_offset_ += size;
					return {BodyReadStatus::Data, size};
				}

				for(;;)
				{
					if(!wait(false)) return {BodyReadStatus::Timeout, 0};

					const auto result = recv(socket_, reinterpret_cast<char *>(destination),
					        static_cast<int>(std::min(capacity, static_cast<std::size_t>(INT_MAX))), 0);
					if(result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;

					if(result > 0) return {BodyReadStatus::Data, static_cast<std::size_t>(result)};

					return {result == 0 ? BodyReadStatus::Disconnected : BodyReadStatus::TransportError, 0};
				}
			}

			ResponseTransport responseTransport() override
			{
				return {this, sendResponse};
			}

			void *extension(const void *key) const override
			{
				if(key == &websocket_exchange_key) return websocket_extension_;

				if(key == &sse::exchange_key)
					return const_cast<sse::Exchange *>(static_cast<const sse::Exchange *>(&sse_exchange_));

				return nullptr;
			}

			bool hasActiveSseStream() const
			{
				return sse_exchange_.active();
			}

			void completeSseCallback()
			{
				sse_exchange_.callbackComplete();
			}

			void rollbackSseStream()
			{
				sse_exchange_.rollback();
			}

			std::size_t headerCount(std::string_view name) const
			{
				return static_cast<std::size_t>(std::count_if(headers_.begin(), headers_.end(),
				       [name](const auto &header) {
					return detail::asciiEqualIgnoreCase(header.first, name);
				}));
			}

			std::string_view headerValue(std::string_view name) const
			{
				for(const auto &header : headers_)
					if(detail::asciiEqualIgnoreCase(header.first, name)) return header.second;

				return {};
			}

			bool sendUpgrade(std::string_view key)
			{
				std::string accept;
				if(!websocketAccept(key, accept)) return false;

				const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
				    "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
				return writeAll(response.data(), response.size());
			}

			void setWebSocketExtension(WebSocketExchange *extension)
			{
				websocket_extension_ = extension;
			}

			const std::string &target() const
			{
				return target_;
			}

			const std::vector<std::pair<std::string, std::string> > &headers() const
			{
				return headers_;
			}

			const std::string &pendingBody() const
			{
				return pending_body_;
			}

			bool isHttp11() const
			{
				return http11_;
			}

		};

		HandlerBase *findWebSocketHandler(HttpRequestHandler &handler, std::string_view target)
		{
			const auto query = target.find('?');
			const auto path = target.substr(0, query);
			const auto routes = handler.transportRoutes();
			for(std::size_t index = 0; index < routes.count; ++index)
			{
				HandlerBase *candidate = routes.data[index];
				if(!candidate) continue;

				const char *uri = candidate->websocket_uri();
				if(uri && path == uri) return candidate;
			}
			return nullptr;
		}

		bool validateWebSocketHandshake(const SocketExchange &exchange)
		{
			const auto upgrade = exchange.headerValue("Upgrade");
			const auto connection = exchange.headerValue("Connection");
			const auto version = exchange.headerValue("Sec-WebSocket-Version");
			const auto key = exchange.headerValue("Sec-WebSocket-Key");
			return exchange.head().method == HttpMethod::Get && exchange.isHttp11() &&
			       exchange.head().content_length == 0 && exchange.headerCount("Upgrade") == 1 &&
			       exchange.headerCount("Connection") == 1 && exchange.headerCount("Sec-WebSocket-Version") == 1 &&
			       exchange.headerCount("Sec-WebSocket-Key") == 1 && hasToken(upgrade, "websocket") &&
			       hasToken(connection, "upgrade") && version == "13" && validWebSocketKey(key);
		}

	}

	class WindowsWebSocketSession final : public WebSocketSession
	{
		struct Message
		{
			std::vector<uint8_t> bytes;
			bool binary;
		};
		std::mutex mutex_;
		WebSocketLimits limits_;
		std::deque<Message> pending_;
		std::size_t pending_count_ = 0;
		std::size_t pending_bytes_ = 0, active_bytes_ = 0;
		bool valid_ = true;

	public:
		explicit WindowsWebSocketSession(const WebSocketLimits &limits)
		  : limits_(limits)
		{
		}

		WebSocketSendResult sendCopy(const uint8_t *data, std::size_t size, bool binary) override
		{
			if(!data && size) return WebSocketSendResult::InvalidArgument;

			if(size > limits_.max_message_size) return WebSocketSendResult::TooLarge;

			std::lock_guard<std::mutex> lock(mutex_);
			if(!valid_) return WebSocketSendResult::Closed;

			if(pending_count_ >= limits_.max_pending_messages ||
			    size > limits_.max_pending_bytes - pending_bytes_) return WebSocketSendResult::QueueFull;

			try
			{
				Message message;
				message.binary = binary;
				if(size) message.bytes.assign(data, data + size);

				pending_.push_back(std::move(message));
				++pending_count_;
				pending_bytes_ += size;
			}
			catch(const std::bad_alloc &) { return WebSocketSendResult::NoMemory; }
			return WebSocketSendResult::Queued;
		}

		void invalidate() override
		{
			std::lock_guard<std::mutex> lock(mutex_);
			valid_ = false;
			pending_.clear();
			pending_count_ = 0;
			pending_bytes_ = active_bytes_ = 0;
		}

		bool take(std::vector<uint8_t> &data, bool &binary)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if(!valid_ || pending_.empty()) return false;

			data = std::move(pending_.front().bytes);
			active_bytes_ = data.size();
			binary = pending_.front().binary;
			pending_.pop_front();
			return true;
		}

		void delivered()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if(pending_count_)
			{
				--pending_count_;
				pending_bytes_ -= active_bytes_;
				active_bytes_ = 0;
			}
		}

	};

	struct WindowsWebSocketConnection
	{
		SOCKET socket = INVALID_SOCKET;
		HandlerBase *endpoint = nullptr;
		WebSocketClient *client = nullptr;
		std::shared_ptr<WindowsWebSocketSession> session;
		WebSocketLimits limits;
		std::string target;
		std::vector<std::pair<std::string, std::string> > headers;
		std::vector<uint8_t> input;
		std::vector<uint8_t> fragmented;
		bool fragmentation_active = false;
		bool fragmented_binary = false;
		bool closing = false;
		bool close_after_write = false;
		std::deque<std::vector<uint8_t> > control_output;
		std::size_t control_output_bytes = 0;
		std::vector<uint8_t> active_output;
		std::size_t output_offset = 0;
		Clock::time_point output_deadline{};
		bool active_application_message = false;

		~WindowsWebSocketConnection()
		{
			if(client) WebSocketEndpoint::disconnect(client);

			client = nullptr;
			if(socket != INVALID_SOCKET) closesocket(socket);

			socket = INVALID_SOCKET;
		}

		void close()
		{
			if(closing) return;

			closing = true;
			close_after_write = true;
			queueControl({0x88, 0x02, 0x03, 0xe8});
		}

		void queueControl(const std::vector<uint8_t> &frame)
		{
			if(frame.size() > 512 || control_output_bytes > 512 - frame.size())
			{
				closing = true;
				close_after_write = true;
				return;
			}

			try
			{
				control_output.push_back(frame);
				control_output_bytes += frame.size();
			}
			catch(const std::bad_alloc &) { closing = true; close_after_write = true; }
		}

		bool prepareWrite()
		{
			if(!active_output.empty()) return true;

			if(!control_output.empty())
			{
				active_output = std::move(control_output.front());
				control_output_bytes -= active_output.size();
				control_output.pop_front();
				active_application_message = false;
				output_offset = 0;
				output_deadline = Clock::now() + std::chrono::seconds(1);
				return true;
			}

			if(closing) return false;

			std::vector<uint8_t> payload;
			bool binary = false;
			if(!session || !session->take(payload, binary)) return false;

			try
			{
				active_output.reserve(payload.size() + 10);
				active_output.push_back(binary ? 0x82 : 0x81);
				if(payload.size() < 126) active_output.push_back(static_cast<uint8_t>(payload.size()));
				else if(payload.size() <= 0xffff)
				{
					active_output.push_back(126);
					active_output.push_back(static_cast<uint8_t>(payload.size() >> 8));
					active_output.push_back(static_cast<uint8_t>(payload.size()));
				}
				else
				{
					active_output.push_back(127);
					for(int shift = 56; shift >= 0; shift -= 8)
						active_output.push_back(static_cast<uint8_t>(static_cast<uint64_t>(payload.size()) >> shift));
				}

				active_output.insert(active_output.end(), payload.begin(), payload.end());
			}
			catch(const std::bad_alloc &)
			{
				active_output.clear();
				closing = true;
				return false;
			}
			active_application_message = true;
			output_offset = 0;
			output_deadline = Clock::now() + std::chrono::seconds(1);
			return true;
		}

		void writeReady()
		{
			if(!prepareWrite()) return;

			if(Clock::now() >= output_deadline)
			{
				active_output.clear();
				control_output.clear();
				control_output_bytes = 0;
				closing = true;
				return;
			}

			const auto remaining = active_output.size() - output_offset;
			const int written = ::send(socket, reinterpret_cast<const char *>(active_output.data() + output_offset),
			        static_cast<int>(std::min(remaining, static_cast<std::size_t>(INT_MAX))), 0);
			if(written == SOCKET_ERROR)
			{
				if(WSAGetLastError() != WSAEWOULDBLOCK)
				{
					active_output.clear();
					control_output.clear();
					control_output_bytes = 0;
					closing = true;
				}

				return;
			}

			if(written <= 0)
			{
				active_output.clear();
				control_output.clear();
				control_output_bytes = 0;
				closing = true;
				return;
			}

			output_offset += static_cast<std::size_t>(written);
			if(output_offset == active_output.size())
			{
				active_output.clear();
				output_offset = 0;
				output_deadline = {};
				if(active_application_message && session) session->delivered();

				active_application_message = false;
			}
		}

		bool appendReceived()
		{
			uint8_t bytes[4096];
			const std::size_t maximum = limits.max_message_size + 14;
			if(input.size() >= maximum)
			{
				close();
				return false;
			}

			const auto capacity = std::min(sizeof(bytes), maximum - input.size());
			const auto received = recv(socket, reinterpret_cast<char *>(bytes), static_cast<int>(capacity), 0);
			if(received == SOCKET_ERROR)
			{
				if(WSAGetLastError() == WSAEWOULDBLOCK) return true;

				closing = true;
				return false;
			}

			if(received <= 0) { closing = true; return false; }

			try { input.insert(input.end(), bytes, bytes + received); }
			catch(const std::bad_alloc &) { closing = true; return false; }
			return true;
		}

	};

	namespace
	{
		bool validUtf8(const uint8_t *data, std::size_t size)
		{
			if(!size) return true;

			return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char *>(data),
			           static_cast<int>(size), nullptr, 0) > 0;
		}

		std::vector<uint8_t> controlFrame(uint8_t opcode, const uint8_t *data, std::size_t size)
		{
			std::vector<uint8_t> frame;
			frame.reserve(size + 2);
			frame.push_back(static_cast<uint8_t>(0x80 | opcode));
			frame.push_back(static_cast<uint8_t>(size));
			if(size) frame.insert(frame.end(), data, data + size);

			return frame;
		}

		class WindowsWebSocketExchange final : public WebSocketExchange, public HttpExchange
		{
			WindowsWebSocketConnection &connection_;
			std::string target_;
			std::vector<std::pair<std::string, std::string> > headers_;
			bool opening_;
			std::vector<uint8_t> delivered_;

			WebSocketReadResult protocolError(uint16_t status)
			{
				const uint8_t code[] = {static_cast<uint8_t>(status >> 8), static_cast<uint8_t>(status)};
				connection_.queueControl(controlFrame(8, code, sizeof(code)));
				connection_.closing = true;
				connection_.close_after_write = true;
				return {WebSocketReadStatus::Error, {}, false};
			}

		public:
			WindowsWebSocketExchange(WindowsWebSocketConnection &connection, std::string target,
			    std::vector<std::pair<std::string, std::string> > headers, bool opening)
			  : connection_(connection), target_(std::move(target)), headers_(std::move(headers)), opening_(opening)
			{
			}

			bool opening() const override
			{
				return opening_;
			}

			std::shared_ptr<WebSocketSession> createSession(const WebSocketLimits &limits) override
			{
				if(limits.max_message_size > std::numeric_limits<std::size_t>::max() - 14) return {};

				try
				{
					connection_.limits = limits;
					connection_.session = std::make_shared<WindowsWebSocketSession>(limits);
					return connection_.session;
				}
				catch(const std::bad_alloc &) { return {}; }
			}

			WebSocketClient *client() const override
			{
				return connection_.client;
			}

			void attach(WebSocketClient *client) override
			{
				connection_.client = client;
			}

			WebSocketReadResult readMessage(const WebSocketLimits &limits) override
			{
				if(opening_) return {WebSocketReadStatus::Error, {}, false};

				const auto message_limit = limits.max_message_size;
				for(unsigned iteration = 0; iteration < 64; ++iteration)
				{
					auto &input = connection_.input;
					if(input.size() < 2)
					{
						const auto before = input.size();
						if(!connection_.appendReceived()) return {WebSocketReadStatus::Error, {}, false};

						if(input.size() == before) return {WebSocketReadStatus::Ignored, {}, false};

						continue;
					}

					const uint8_t first = input[0], second = input[1];
					const bool final = (first & 0x80) != 0;
					const uint8_t opcode = first & 0x0f;
					if((first & 0x70) || !(second & 0x80)) return protocolError(1002);

					std::size_t header_size = 2;
					uint64_t length = second & 0x7f;
					if(length == 126)
					{
						header_size += 2;
						if(input.size() < header_size)
						{
							const auto before = input.size();
							if(!connection_.appendReceived()) return {WebSocketReadStatus::Error, {}, false};

							if(input.size() == before) return {WebSocketReadStatus::Ignored, {}, false};

							continue;
						}

						length = (static_cast<uint64_t>(input[2]) << 8) | input[3];
						if(length < 126) return protocolError(1002);
					}
					else if(length == 127)
					{
						header_size += 8;
						if(input.size() < header_size)
						{
							const auto before = input.size();
							if(!connection_.appendReceived()) return {WebSocketReadStatus::Error, {}, false};

							if(input.size() == before) return {WebSocketReadStatus::Ignored, {}, false};

							continue;
						}

						if(input[2] & 0x80) return protocolError(1002);

						length = 0;
						for(unsigned index = 0; index < 8; ++index)
							length = (length << 8) | input[2 + index];
						if(length < 65536) return protocolError(1002);
					}

					const bool control = (opcode & 0x08) != 0;
					if((control && (!final || length > 125)) || length > message_limit)
						return protocolError(length > message_limit ? 1009 : 1002);

					header_size += 4;
					if(length > std::numeric_limits<std::size_t>::max() - header_size)
						return protocolError(1009);

					const auto frame_size = header_size + static_cast<std::size_t>(length);
					if(input.size() < frame_size)
					{
						const auto before = input.size();
						if(!connection_.appendReceived()) return {WebSocketReadStatus::Error, {}, false};

						if(input.size() == before) return {WebSocketReadStatus::Ignored, {}, false};

						continue;
					}

					std::array<uint8_t, 4> mask{};
					std::copy_n(input.data() + header_size - 4, 4, mask.data());
					std::vector<uint8_t> payload;
					try
					{
						payload.resize(static_cast<std::size_t>(length));
						for(std::size_t index = 0; index < payload.size(); ++index)
							payload[index] = input[header_size + index] ^ mask[index & 3];
						input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(frame_size));
					}
					catch(const std::bad_alloc &) { return protocolError(1011); }
					if(control)
					{
						if(opcode == 9)
						{
							connection_.queueControl(controlFrame(10, payload.data(), payload.size()));
							return {WebSocketReadStatus::Ignored, {}, false};
						}

						if(opcode == 10) return {WebSocketReadStatus::Ignored, {}, false};

						if(opcode != 8 || payload.size() == 1) return protocolError(1002);

						if(payload.size() >= 2)
						{
							const uint16_t status = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
							const bool allowed = status == 1000 || status == 1001 || status == 1002 || status == 1003 ||
							    (status >= 1007 && status <= 1014) || (status >= 3000 && status <= 4999);
							if(!allowed) return protocolError(1002);

							if(!validUtf8(payload.data() + 2, payload.size() - 2)) return protocolError(1007);
						}

						connection_.queueControl(controlFrame(8, payload.data(), payload.size()));
						connection_.closing = true;
						connection_.close_after_write = true;
						return {WebSocketReadStatus::Closed, {}, false};
					}

					bool binary = false;
					if(opcode == 1 || opcode == 2)
					{
						if(connection_.fragmentation_active) return protocolError(1002);

						binary = opcode == 2;
						if(final)
						{
							if(!binary && !validUtf8(payload.data(), payload.size())) return protocolError(1007);

							try { delivered_ = std::move(payload); }
							catch(const std::bad_alloc &) { return protocolError(1011); }
							return {WebSocketReadStatus::Message, {delivered_.data(), delivered_.size()}, binary};
						}

						connection_.fragmentation_active = true;
						connection_.fragmented_binary = binary;
						connection_.fragmented = std::move(payload);
						continue;
					}

					if(opcode != 0 || !connection_.fragmentation_active) return protocolError(1002);

					if(payload.size() > message_limit - connection_.fragmented.size())
						return protocolError(1009);

					try { connection_.fragmented.insert(connection_.fragmented.end(), payload.begin(), payload.end()); }
					catch(const std::bad_alloc &) { return protocolError(1011); }
					if(!final) continue;

					binary = connection_.fragmented_binary;
					connection_.fragmentation_active = false;
					if(!binary && !validUtf8(connection_.fragmented.data(), connection_.fragmented.size()))
						return protocolError(1007);

					delivered_ = std::move(connection_.fragmented);
					connection_.fragmented.clear();
					return {WebSocketReadStatus::Message, {delivered_.data(), delivered_.size()}, binary};
				}
				return {WebSocketReadStatus::Ignored, {}, false};
			}

			void close() override
			{
				connection_.close();
			}

			HttpRequestHead head() const override
			{
				return {HttpMethod::Get, target_, 0};
			}

			HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const override
			{
				if(!name || !*name || (!destination && capacity))
					return {HeaderReadStatus::InvalidArgument, 0};

				for(const auto &header : headers_)
					if(detail::asciiEqualIgnoreCase(header.first, name))
					{
						if(capacity <= header.second.size())
							return {HeaderReadStatus::BufferTooSmall, header.second.size()};

						std::memcpy(destination, header.second.c_str(), header.second.size() + 1);
						return {HeaderReadStatus::Ok, header.second.size()};
					}

				return {HeaderReadStatus::NotFound, 0};
			}

			BodyReadResult readBody(uint8_t *, std::size_t) override
			{
				return {BodyReadStatus::End, 0};
			}

			ResponseTransport responseTransport() override
			{
				return {nullptr, [](void *, int, const char *, const Header *, std::size_t, ByteView)
						{
							return SendResult::TransportError;
						}};
			}

			void *extension(const void *key) const override
			{
				return key == &websocket_exchange_key ? const_cast<WindowsWebSocketExchange *>(this) : nullptr;
			}

		};
	}

	WindowsHttpBackend::~WindowsHttpBackend()
	{
		(void)stop();
	}

	HttpError WindowsHttpBackend::start(HttpRequestHandler &handler, const HttpLimits &limits)
	{
		if(handler_) return HttpError::InvalidState;

		if(!limits.max_connections || !limits.exchange_timeout_ms || limits.max_header_bytes < 4 ||
		    !limits.max_headers || !limits.max_target_size || limits.max_connections >= FD_SETSIZE)
			return HttpError::InvalidArgument;

		try
		{
			sse_streams_.clear();
			sse_streams_.reserve(limits.max_connections);
		}
		catch(const std::bad_alloc &) { return HttpError::TransportError; }

		WSADATA data{};
		if(WSAStartup(MAKEWORD(2, 2), &data) != 0) return HttpError::TransportError;

		const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if(listener == INVALID_SOCKET) { WSACleanup(); return HttpError::TransportError; }

		const BOOL exclusive = TRUE;
		(void)setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive),
		    sizeof(exclusive));
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(requested_port_);
		u_long nonblocking = 1;
		int address_size = sizeof(address);
		if(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
		    listen(listener, limits.max_connections) != 0 || ioctlsocket(listener, FIONBIO, &nonblocking) != 0 ||
		    getsockname(listener, reinterpret_cast<sockaddr *>(&address), &address_size) != 0)
		{
			closesocket(listener);
			WSACleanup();
			return HttpError::TransportError;
		}

		listener_ = listener;
		bound_port_ = ntohs(address.sin_port);
		handler_ = &handler;
		limits_ = limits;
		stopping_ = false;
		try { worker_ = std::thread(&WindowsHttpBackend::run, this); }
		catch(...)
		{
			handler_ = nullptr;
			closesocket(listener);
			listener_ = invalid_socket;
			WSACleanup();
			return HttpError::TransportError;
		}
		return HttpError::Ok;
	}

	HttpError WindowsHttpBackend::stop()
	{
		if(!handler_) return HttpError::Ok;

		if(worker_.get_id() == std::this_thread::get_id()) return HttpError::InvalidState;

		{
			std::lock_guard<std::mutex> lock(mutex_);
			if(!sse_streams_.empty()) return HttpError::InvalidState;

			stopping_ = true;
			if(active_ != invalid_socket) (void)shutdown(static_cast<SOCKET>(active_), SD_BOTH);
		}
		worker_.join();
		websocket_connections_.clear();
		closesocket(static_cast<SOCKET>(listener_));
		listener_ = invalid_socket;
		handler_ = nullptr;
		bound_port_ = 0;
		WSACleanup();
		return HttpError::Ok;
	}

	bool WindowsHttpBackend::registerSseStream(const std::shared_ptr<sse::Stream> &stream)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if(!handler_ || stopping_ || !stream ||
		    websocket_connections_.size() + sse_streams_.size() + 1 > limits_.max_connections)
			return false;

		try
		{
			sse_streams_.push_back(stream);
			return true;
		}
		catch(const std::bad_alloc &) { return false; }
	}

	void WindowsHttpBackend::releaseSseStream(const sse::Stream *stream)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto found = std::find_if(sse_streams_.begin(), sse_streams_.end(),
		        [stream](const auto &candidate) {
			return candidate.get() == stream;
		});
		if(found != sse_streams_.end()) sse_streams_.erase(found);
	}

	void WindowsHttpBackend::run()
	{
		while(!stopping_)
		{
			fd_set readable, writable;
			FD_ZERO(&readable);
			FD_ZERO(&writable);
			FD_SET(static_cast<SOCKET>(listener_), &readable);
			for(const auto &connection : websocket_connections_)
			{
				if(connection->socket == INVALID_SOCKET) continue;

				if(!connection->active_output.empty() && Clock::now() >= connection->output_deadline)
				{
					connection->active_output.clear();
					connection->control_output.clear();
					connection->control_output_bytes = 0;
					connection->closing = true;
				}

				if(!connection->closing) FD_SET(connection->socket, &readable);

				if(connection->prepareWrite()) FD_SET(connection->socket, &writable);
			}
			timeval timeout{0, 100000};
			const int selected = select(0, &readable, &writable, nullptr, &timeout);
			if(selected == SOCKET_ERROR) continue;

			for(auto &connection : websocket_connections_)
			{
				if(connection->socket == INVALID_SOCKET) continue;

				try
				{
					if(!connection->closing &&
					    (FD_ISSET(connection->socket, &readable) || !connection->input.empty()))
					{
						WindowsWebSocketExchange exchange(*connection, connection->target, connection->headers, false);
						Request request(exchange);
						Response response(exchange.responseTransport());
						connection->endpoint->handleRequest(&request, response);
					}

					if(FD_ISSET(connection->socket, &writable)) connection->writeReady();
				}
				catch(...) { connection->closing = true; }
			}
			websocket_connections_.erase(std::remove_if(websocket_connections_.begin(), websocket_connections_.end(),
			    [](const auto &connection)
			{
				return connection->closing && connection->active_output.empty() &&
				       connection->control_output.empty();
			}), websocket_connections_.end());

			if(!FD_ISSET(static_cast<SOCKET>(listener_), &readable)) continue;

			const auto client = accept(static_cast<SOCKET>(listener_), nullptr, nullptr);
			if(client == INVALID_SOCKET) continue;

			u_long nonblocking = 1;
			if(ioctlsocket(client, FIONBIO, &nonblocking) != 0) { closesocket(client); continue; }

			bool connection_limit_reached = false;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				connection_limit_reached = websocket_connections_.size() + sse_streams_.size() >=
				    limits_.max_connections;
			}
			if(connection_limit_reached)
			{
				const std::string rejection =
				    "HTTP/1.1 503 Response\r\nConnection: close\r\nContent-Length: 24\r\n\r\nConnection limit reached";
				(void)::send(client, rejection.data(), static_cast<int>(rejection.size()), 0);
				closesocket(client);
				continue;
			}

			{
				std::lock_guard<std::mutex> lock(mutex_);
				if(stopping_) { closesocket(client); break; }

				active_ = client;
			}
			bool upgraded = false;
			bool socket_transferred = false;
			SocketExchange exchange(client, limits_, stopping_, *this);
			try
			{
				const auto status = exchange.receiveHead();
				if(status)
				{
					Response response(exchange.responseTransport());
					(void)response.text(status, "HTTP request rejected");
				}
				else
				{
					HandlerBase *endpoint = findWebSocketHandler(*handler_, exchange.target());
					const bool upgrade_requested = hasToken(exchange.headerValue("Upgrade"), "websocket");
					if(endpoint && upgrade_requested)
					{
						if(!validateWebSocketHandshake(exchange))
						{
							Response response(exchange.responseTransport());
							(void)response.text(400, "Invalid WebSocket handshake");
						}
						else if(exchange.sendUpgrade(exchange.headerValue("Sec-WebSocket-Key")))
						{
							auto connection = std::make_shared<WindowsWebSocketConnection>();
							connection->socket = client;
							socket_transferred = true;
							connection->endpoint = endpoint;
							connection->target = exchange.target();
							connection->headers = exchange.headers();
							WindowsWebSocketExchange websocket_exchange(*connection, connection->target,
							    connection->headers, true);
							exchange.setWebSocketExtension(&websocket_exchange);
							Request request(exchange);
							Response response(exchange.responseTransport());
							endpoint->handleRequest(&request, response);
							const auto &pending = exchange.pendingBody();
							const auto pending_limit = connection->limits.max_message_size <=
							    std::numeric_limits<std::size_t>::max() - 14
							? connection->limits.max_message_size + 14 : 0;
							if(pending.size() <= pending_limit)
								connection->input.assign(pending.begin(), pending.end());
							else connection->close();

							websocket_connections_.push_back(std::move(connection));
							upgraded = true;
						}
						else
						{
							Response response(exchange.responseTransport());
							(void)response.text(500, "WebSocket handshake failed");
						}
					}
					else
					{
						exchange.drainSmallRejectedBody();
						(void)handler_->handle(exchange);
					}
				}
			}
			catch(...) { /* Never unwind through a native worker entry point. */ }
			const bool sse_transferred = exchange.hasActiveSseStream();
			if(!upgraded && !socket_transferred && !sse_transferred)
			{
				finishConnection(client, stopping_);
				closesocket(client);
			}

			{
				std::lock_guard<std::mutex> lock(mutex_);
				active_ = invalid_socket;
			}
			if(sse_transferred) exchange.completeSseCallback();
		}
		websocket_connections_.clear();
	}

}
