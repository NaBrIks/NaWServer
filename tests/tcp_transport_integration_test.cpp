// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "support/host_sockets.h"
#include <NaWServer/backends/tcp.h>
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/handler_middleware.h>
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/sse.h>
#include <NaWServer/websocket.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sockets = NaWServer::test::host;

using namespace NaWServer;

namespace
{

	using Clock = std::chrono::steady_clock;
	std::atomic<unsigned> checks{0};

	void check(bool value, int line)
	{
		checks.fetch_add(1);
		if(!value)
		{
			std::printf("TCP_TRANSPORT_INTEGRATION_FAIL line=%d native=%d\n", line, sockets::lastError());
			std::fflush(stdout);
			std::_Exit(1);
		}
	}

#define CHECK(value) check((value), __LINE__)

	template <class Predicate> void until(Predicate predicate)
	{
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		while(!predicate())
		{
			CHECK(Clock::now() < deadline);
			std::this_thread::yield();
		}
	}

	// Metadata only is locked. Every native call executes outside this mutex,
	// including calls on the independent HTTP/WS and SSE owner threads.
	class CappedTransport final : public tcp::Transport
	{
		struct Record
		{
			tcp::Handle handle;
			std::uint64_t serial = 0;
			std::size_t written = 0;
			unsigned reads = 0;
			unsigned writes = 0;
			std::optional<std::size_t> fail_after;
			bool closing = false;
			bool fault_reported = false;
		};
		mutable std::mutex mutex_;
		std::array<Record, 16> records_{};
		std::size_t cap_;
		std::uint64_t serial_ = 0;
		std::optional<std::size_t> next_failure_;
		std::size_t injected_blocks_ = 0;
		std::size_t injected_interrupts_ = 0;
		std::size_t fault_count_ = 0;
		std::size_t fault_bytes_ = 0;
		std::size_t max_read_ = 0;
		std::size_t max_write_ = 0;
		std::size_t shutdown_calls_ = 0;

		Record *find(tcp::Handle handle, bool include_closing = false)
		{
			Record *closing = nullptr;
			for(auto &record : records_)
			{
				if(!record.handle.valid || record.handle.value != handle.value)
					continue;

				if(!record.closing)
					return &record;

				if(include_closing)
					closing = &record;
			}
			return closing;
		}

		Record *findSerial(std::uint64_t serial)
		{
			for(auto &record : records_)
				if(record.handle.valid && record.serial == serial)
					return &record;

			return nullptr;
		}

		std::optional<tcp::IoResult> inject(unsigned call)
		{
			if(call % 11 == 0)
			{
				++injected_interrupts_;
				return tcp::IoResult{tcp::IoStatus::Interrupted, 0, sockets::interrupted_error};
			}

			if(call % 7 == 0)
			{
				++injected_blocks_;
				return tcp::IoResult{tcp::IoStatus::WouldBlock, 0, sockets::would_block_error};
			}

			return std::nullopt;
		}

	public:
		sockets::TcpTransport native;

		explicit CappedTransport(std::size_t cap)
		  : cap_(cap)
		{
		}

		tcp::ListenerResult openListener(const char *address, std::uint16_t port, int backlog) override
		{
			return native.openListener(address, port, backlog);
		}

		tcp::AcceptResult accept(tcp::Handle listener) override
		{
			const auto result = native.accept(listener);
			if(result.handle.valid)
			{
				std::lock_guard<std::mutex> lock(mutex_);
				auto position = std::find_if(records_.begin(), records_.end(), [](const Record &record)
				{
					return !record.handle.valid;
				});
				CHECK(position != records_.end());
				*position = {};
				position->handle = result.handle;
				position->serial = ++serial_;
				position->fail_after = std::exchange(next_failure_, std::nullopt);
			}

			return result;
		}

		tcp::IoResult readSome(tcp::Handle handle, std::uint8_t *destination, std::size_t capacity) override
		{
			std::size_t requested;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if(auto *record = find(handle))
					if(const auto result = inject(++record->reads))
						return *result;

				requested = std::min(capacity, cap_);
			}
			const auto result = native.readSome(handle, destination, requested);
			if(result.status == tcp::IoStatus::Progress)
			{
				std::lock_guard<std::mutex> lock(mutex_);
				max_read_ = std::max(max_read_, result.size);
			}

			return result;
		}

		tcp::IoResult writeSome(tcp::Handle handle, const std::uint8_t *bytes, std::size_t size) override
		{
			std::size_t requested;
			std::uint64_t serial = 0;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				requested = std::min(size, cap_);
				if(auto *record = find(handle))
				{
					serial = record->serial;
					if(record->fail_after)
					{
						if(record->written >= *record->fail_after)
						{
							if(!record->fault_reported)
							{
								record->fault_reported = true;
								++fault_count_;
								fault_bytes_ = record->written;
							}

							return {tcp::IoStatus::Fatal, 0, sockets::aborted_error};
						}

						requested = std::min(requested, *record->fail_after - record->written);
					}

					if(const auto result = inject(++record->writes))
						return *result;
				}
			}
			const auto result = native.writeSome(handle, bytes, requested);
			if(result.status == tcp::IoStatus::Progress)
			{
				std::lock_guard<std::mutex> lock(mutex_);
				max_write_ = std::max(max_write_, result.size);
				if(auto *record = findSerial(serial))
					record->written += result.size;
			}

			return result;
		}

		tcp::WaitResult wait(const tcp::Interest *interests, tcp::Ready *ready,
		    std::size_t count, std::uint32_t timeout_ms) override
		{
			return native.wait(interests, ready, count, timeout_ms);
		}

		tcp::OperationResult shutdownWrite(tcp::Handle handle) override
		{
			{
				std::lock_guard<std::mutex> lock(mutex_);
				++shutdown_calls_;
			}
			return native.shutdownWrite(handle);
		}

		tcp::CloseResult close(tcp::Handle &handle) override
		{
			std::uint64_t serial = 0;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if(auto *record = find(handle, true))
				{
					record->closing = true;
					serial = record->serial;
				}
			}
			const auto result = native.close(handle);
			if(result.released && serial)
			{
				std::lock_guard<std::mutex> lock(mutex_);
				// A different owner can accept a reused native value before this
				// bookkeeping step. Only erase this close's captured lifetime.
				if(auto *record = findSerial(serial))
					*record = {};
			}

			return result;
		}

		void failNextAcceptedWriteAfter(std::size_t bytes)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			CHECK(!next_failure_);
			next_failure_ = bytes;
		}

		std::size_t activeAccepted() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return static_cast<std::size_t>(std::count_if(records_.begin(), records_.end(), [](const Record &record)
			{
				return record.handle.valid;
			}));
		}

		std::size_t faultBytes() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return fault_bytes_;
		}

		std::size_t faultCount() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return fault_count_;
		}

		void verifyCaps() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			CHECK(max_read_ > 0 && max_read_ <= cap_ && max_write_ > 0 && max_write_ <= cap_);
			CHECK(injected_blocks_ > 0 && injected_interrupts_ > 0);
			std::printf(
				"TCP_TRANSPORT cap=%zu would_block=%zu interrupted=%zu native_max_read=%zu native_max_write=%zu\n",
				cap_, injected_blocks_, injected_interrupts_, max_read_, max_write_);
		}

		void verifyNoHalfClose() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			CHECK(shutdown_calls_ == 0);
		}

	};

	struct Socket
	{
		sockets::Handle value = sockets::invalid;
		Socket() = default;
		explicit Socket(sockets::Handle value)
		  : value(value)
		{
		}

		Socket(const Socket &) = delete;
		Socket &operator=(const Socket &) = delete;
		Socket(Socket &&other) noexcept
		  : value(std::exchange(other.value, sockets::invalid))
		{
		}

		Socket &operator=(Socket &&other) noexcept
		{
			if(this != &other)
			{
				close();
				value = std::exchange(other.value, sockets::invalid);
			}

			return *this;
		}

		~Socket()
		{
			close();
		}

		void close()
		{
			if(value != sockets::invalid)
			{
				CHECK(sockets::closeSocket(value) == 0);
				value = sockets::invalid;
			}
		}

	};

	Socket connectTo(std::uint16_t port)
	{
		Socket socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
		CHECK(socket.value != sockets::invalid);
		CHECK(sockets::setTimeouts(socket.value, 3000));
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);
		CHECK(connect(socket.value, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
		return socket;
	}

	void sendAll(const Socket &socket, const std::uint8_t *bytes, std::size_t size)
	{
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		while(size)
		{
			CHECK(Clock::now() < deadline);
			const int count = sockets::sendSome(socket.value, reinterpret_cast<const char *>(bytes),
			        static_cast<int>(size), 0);
			CHECK(count > 0);
			bytes += count;
			size -= static_cast<std::size_t>(count);
		}
	}

	void sendAll(const Socket &socket, const std::string &bytes)
	{
		sendAll(socket, reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
	}

	std::string readUntil(const Socket &socket, std::string_view marker)
	{
		std::string result;
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		while(result.find(marker) == result.npos)
		{
			CHECK(Clock::now() < deadline && result.size() < 16384);
			char byte;
			CHECK(sockets::receiveSome(socket.value, &byte, 1, 0) == 1);
			result.push_back(byte);
		}
		return result;
	}

	std::string readToEnd(const Socket &socket)
	{
		std::string result;
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		for(;;)
		{
			CHECK(Clock::now() < deadline && result.size() < 16384);
			char bytes[1024];
			const int count = sockets::receiveSome(socket.value, bytes, sizeof(bytes), 0);
			CHECK(count >= 0);
			if(count == 0)
				return result;

			result.append(bytes, static_cast<std::size_t>(count));
		}
	}

	std::string http(std::uint16_t port, const std::string &request)
	{
		auto socket = connectTo(port);
		sendAll(socket, request);
		// Keep the client write side open: EOF must come from the server's close,
		// even though bounded pre-close discard cannot receive a peer FIN.
		return readToEnd(socket);
	}

	std::string body(const std::string &response)
	{
		const auto delimiter = response.find("\r\n\r\n");
		CHECK(delimiter != response.npos);
		return response.substr(delimiter + 4);
	}

	constexpr std::string_view handshake_response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
	    "Connection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
	constexpr char handshake_request[] = "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
	    "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
	    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";

	Socket websocket(std::uint16_t port)
	{
		auto socket = connectTo(port);
		sendAll(socket, handshake_request);
		CHECK(readUntil(socket, "\r\n\r\n") == handshake_response);
		return socket;
	}

	void sendFrame(const Socket &socket, std::uint8_t opcode, ByteView bytes, bool final = true)
	{
		CHECK(bytes.size() <= 4096);
		std::vector<std::uint8_t> wire;
		wire.push_back(static_cast<std::uint8_t>((final ? 0x80 : 0) | opcode));
		if(bytes.size() < 126)
			wire.push_back(static_cast<std::uint8_t>(0x80 | bytes.size()));
		else
		{
			wire.push_back(0x80 | 126);
			wire.push_back(static_cast<std::uint8_t>(bytes.size() >> 8));
			wire.push_back(static_cast<std::uint8_t>(bytes.size()));
		}

		constexpr std::uint8_t mask[]{0x12, 0x34, 0x56, 0x78};
		wire.insert(wire.end(), std::begin(mask), std::end(mask));
		for(std::size_t index = 0; index < bytes.size(); ++index)
			wire.push_back(bytes.data()[index] ^ mask[index % 4]);

		sendAll(socket, wire.data(), wire.size());
	}

	void receiveExact(const Socket &socket, std::uint8_t *bytes, std::size_t size)
	{
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		while(size)
		{
			CHECK(Clock::now() < deadline);
			const int received = sockets::receiveSome(socket.value, reinterpret_cast<char *>(bytes),
			        static_cast<int>(size), 0);
			CHECK(received > 0);
			bytes += received;
			size -= static_cast<std::size_t>(received);
		}
	}

	struct Frame
	{
		std::uint8_t opcode;
		std::vector<std::uint8_t> payload;
	};

	Frame receiveFrame(const Socket &socket)
	{
		std::uint8_t header[2];
		receiveExact(socket, header, sizeof(header));
		CHECK((header[0] & 0xf0) == 0x80 && !(header[1] & 0x80));
		std::size_t length = header[1] & 0x7f;
		CHECK(length != 127);
		if(length == 126)
		{
			std::uint8_t extended[2];
			receiveExact(socket, extended, sizeof(extended));
			length = (static_cast<std::size_t>(extended[0]) << 8) | extended[1];
		}

		CHECK(length <= 4096);
		Frame frame{static_cast<std::uint8_t>(header[0] & 0x0f), std::vector<std::uint8_t>(length)};
		if(length)
			receiveExact(socket, frame.payload.data(), length);

		return frame;
	}

	class Application;
	Application *application = nullptr;
	SendResult health(Response &response);
	SendResult echo(Request &request, Response &response);
	SendResult parameter(Request &request, Response &response);
	SendResult openEvents(Request &request, Response &response);

	struct AllocationLedger
	{
		std::atomic<std::size_t> bytes{0}, blocks{0};
		static void *allocate(void *context, std::size_t size)
		{
			auto &ledger = *static_cast<AllocationLedger *>(context);
			void *storage = std::malloc(size);
			if(storage) { ledger.bytes.fetch_add(size); ledger.blocks.fetch_add(1); }

			return storage;
		}

		static void release(void *context, void *storage, std::size_t size)
		{
			auto &ledger = *static_cast<AllocationLedger *>(context);
			CHECK(ledger.bytes.fetch_sub(size) >= size && ledger.blocks.fetch_sub(1) > 0);
			std::free(storage);
		}

		BufferAllocator allocator()
		{
			return {this, allocate, release};
		}

	};

	TcpOptions options(bool managed = false, BufferAllocator allocator = {})
	{
		TcpOptions result;
		result.port = 0;
		result.websocket_send_timeout_ms = 3000;
		result.http_memory.enabled = managed;
		result.buffer_memory.allocator = allocator;
		return result;
	}

	sse::Limits sseLimits(std::size_t initial_clients, BufferAllocator allocator)
	{
		sse::Limits result;
		result.initial_clients = initial_clients;
		result.buffer_memory.allocator = allocator;
		return result;
	}

	WebSocketLimits websocketLimits(unsigned clients)
	{
		WebSocketLimits limits;
		limits.max_clients = clients;
		return limits;
	}

	class Application
	{
		std::mutex token_mutex_;
		sse::Token last_token_;

	public:
		AllocationLedger tcp_allocations, sse_allocations;
		CappedTransport transport;
		TcpHttpBackend backend;
		sse::TcpAdapter sse_backend;
		sse::EventSource events;
		WebSocketEndpoint endpoint;
		FixedBasicAuthenticator credentials{"user", "password", 1};
		AuthMiddleware auth{credentials};
		AccessMiddleware access;
		HeaderChain<AuthMiddleware, AccessMiddleware> chain{auth, access};
		Route health_route{"/health", HttpMethod::Get, health};
		Route echo_route{"/echo", HttpMethod::Post, echo};
		Route parameter_route{"/parameters", Route::any_method, parameter};
		Route private_route{"/private", HttpMethod::Get, parameter, {}, AccessPolicy::require(1), {}, chain};
		Route events_route{"/events", HttpMethod::Get, openEvents};
		HandlerBase *const routes[6]{&endpoint, &health_route, &echo_route, &parameter_route,
			                         &private_route, &events_route};
		HttpServer server;
		std::atomic<unsigned> connections{0};
		std::atomic<unsigned> messages{0};
		std::atomic<unsigned> opened_events{0};
		std::atomic<unsigned> http_calls{0};

		explicit Application(std::size_t cap, unsigned endpoint_clients = 8, bool managed = false,
		    std::size_t initial_sse_clients = 0)
		  : transport(cap), backend(transport, options(managed, tcp_allocations.allocator())),
		    sse_backend(backend, sseLimits(initial_sse_clients, sse_allocations.allocator())), events(sse_backend),
		    endpoint("/ws", websocketLimits(endpoint_clients)), server(backend, routes)
		{
			application = this;
			endpoint.onConnect([this](WebSocketClient *) {
				connections.fetch_add(1);
			});
			endpoint.onMessage([this](WebSocketClient *client, const std::uint8_t *bytes, std::size_t size, bool)
			{
				messages.fetch_add(1);
				CHECK(client->send_binary(bytes, size) == WebSocketSendResult::Queued);
			});
			CHECK(transport.native.start().status == tcp::Status::Ok);
			start();
		}

		~Application()
		{
			CHECK(events.stop(3000));
			CHECK(server.end() == HttpError::Ok);
			CHECK(transport.activeAccepted() == 0 && transport.native.ownedHandles() == 0);
			transport.verifyNoHalfClose();
			CHECK(transport.native.stop().status == tcp::Status::Ok);
			application = nullptr;
		}

		void start()
		{
			CHECK(events.start());
			CHECK(server.begin() == HttpError::Ok && backend.port() != 0);
		}

		void setToken(sse::Token token)
		{
			std::lock_guard<std::mutex> lock(token_mutex_);
			last_token_ = token;
			opened_events.fetch_add(1);
		}

		sse::Token token()
		{
			std::lock_guard<std::mutex> lock(token_mutex_);
			return last_token_;
		}

		void drained()
		{
			until([this] {
				return endpoint.count() == 0 && transport.activeAccepted() == 0;
			});
			CHECK(transport.native.ownedHandles() == 1);
		}

		void stopHttp()
		{
			HttpError result = HttpError::InvalidState;
			until([&]
			{
				result = server.end();
				CHECK(result == HttpError::Ok || result == HttpError::InvalidState);
				return result == HttpError::Ok;
			});
			CHECK(transport.native.ownedHandles() == 0 && endpoint.count() == 0);
		}

	};

	SendResult health(Response &response)
	{
		application->http_calls.fetch_add(1);
		return response.text(200, "healthy");
	}

	SendResult echo(Request &request, Response &response)
	{
		application->http_calls.fetch_add(1);
		return response.bytes(200, request.body());
	}

	SendResult parameter(Request &request, Response &response)
	{
		application->http_calls.fetch_add(1);
		return response.text(200, request.parameter("value").value_or(""));
	}

	SendResult openEvents(Request &request, Response &response)
	{
		const auto result = application->events.open(request, response);
		if(result.active)
			application->setToken(result.token);

		return result.result;
	}

	Socket eventStream(Application &app)
	{
		auto socket = connectTo(app.backend.port());
		const auto before = app.opened_events.load();
		sendAll(socket, "GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n");
		const auto header = readUntil(socket, "\r\n\r\n");
		CHECK(header.find("200 ") != header.npos);
		CHECK(header.find("text/event-stream") != header.npos);
		until([&] {
			return app.opened_events.load() == before + 1;
		});
		return socket;
	}

	void publish(Application &app, sse::Token token, const sse::Frame &frame)
	{
		until([&]
		{
			const auto result = app.events.publish(token, frame);
			CHECK(result == sse::Publish::Queued || result == sse::Publish::Busy);
			return result == sse::Publish::Queued;
		});
	}

	void mixedCaps(std::size_t cap)
	{
		Application app(cap);
		auto peer = websocket(app.backend.port());
		until([&] {
			return app.connections.load() == 1;
		});
		auto events = eventStream(app);
		const auto old_token = app.token();
		std::string binary(129, '\0');
		for(std::size_t index = 0; index < binary.size(); ++index)
			binary[index] = static_cast<char>(index);

		const auto response = http(app.backend.port(), "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
		        std::to_string(binary.size()) + "\r\n\r\n" + binary);
		CHECK(response.find("200 ") != response.npos && body(response) == binary);
		CHECK(body(http(app.backend.port(),
		    "GET /parameters?value=query%00tail HTTP/1.1\r\nHost: localhost\r\n\r\n")) ==
		    std::string("query\0tail", 10));
		CHECK(http(app.backend.port(),
		    "GET /private HTTP/1.1\r\nHost: localhost\r\n\r\n").find("401 ") != std::string::npos);
		CHECK(body(http(app.backend.port(), "GET /private?value=allowed HTTP/1.1\r\nHost: localhost\r\n"
		    "Authorization: Basic dXNlcjpwYXNzd29yZA==\r\n\r\n")) == "allowed");

		const auto *bytes = reinterpret_cast<const std::uint8_t *>(binary.data());
		sendFrame(peer, 2, {bytes, 63}, false);
		const std::uint8_t ping[]{0, 0xfe, 1};
		sendFrame(peer, 9, {ping, sizeof(ping)});
		sendFrame(peer, 0, {bytes + 63, binary.size() - 63});
		const auto pong = receiveFrame(peer);
		CHECK(pong.opcode == 10 && pong.payload == std::vector<std::uint8_t>(std::begin(ping), std::end(ping)));
		const auto echoed = receiveFrame(peer);
		CHECK(echoed.opcode == 2 && echoed.payload == std::vector<std::uint8_t>(bytes, bytes + binary.size()));
		CHECK(app.messages.load() == 1);
		std::vector<std::uint8_t> source{0, 1, 0xff, 4, 5, 6, 7};
		const auto expected = source;
		until([&]
		{
			const auto broadcast = app.endpoint.send_binary(source.data(), source.size());
			CHECK(broadcast.queued == 1 ||
			(broadcast.rejected == 1 && broadcast.first_error == WebSocketSendResult::Busy));
			return broadcast.queued == 1;
		});
		std::fill(source.begin(), source.end(), std::uint8_t{0x77});
		CHECK(receiveFrame(peer).payload == expected);

		std::string event_data = "owned event";
		sse::Frame frame;
		CHECK(sse::encode({event_data, "integration", std::nullopt, std::nullopt}, frame) == sse::EncodeResult::Ok);
		publish(app, old_token, frame);
		event_data.assign("overwritten");
		frame.bytes.fill('x');
		// Leave WS and SSE output unread while the HTTP owner handles another
		// request; SSE writes concurrently from its independent owner.
		sendFrame(peer, 2, {bytes, binary.size()});
		CHECK(body(http(app.backend.port(), "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")) == "healthy");
		CHECK(receiveFrame(peer).payload == std::vector<std::uint8_t>(bytes, bytes + binary.size()));
		CHECK(app.messages.load() == 2);
		const auto event = readUntil(events, "\n\n");
		CHECK(event.find("owned event") != event.npos && event.find("integration") != event.npos);

		CHECK(app.server.end() == HttpError::InvalidState);
		CHECK(body(http(app.backend.port(), "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")) == "healthy");
		CHECK(app.events.stop(3000));
		CHECK(readToEnd(events).empty());
		app.stopHttp();
		CHECK(readToEnd(peer).empty());
		const auto callback_connections = app.connections.load();
		const auto callback_messages = app.messages.load();
		const auto callback_http = app.http_calls.load();
		peer.close();
		events.close();
		app.start();
		until([&]
		{
			const auto result = app.events.publish(old_token, frame);
			CHECK(result == sse::Publish::Closed || result == sse::Publish::Busy);
			return result == sse::Publish::Closed;
		});
		auto restarted = eventStream(app);
		CHECK(!(app.token() == old_token));
		CHECK(sse::encode({"after restart", std::nullopt, std::nullopt, std::nullopt}, frame) == sse::EncodeResult::Ok);
		publish(app, app.token(), frame);
		CHECK(readUntil(restarted, "\n\n").find("after restart") != std::string::npos);
		CHECK(app.connections.load() == callback_connections && app.messages.load() == callback_messages);
		CHECK(app.http_calls.load() == callback_http);
		CHECK(app.events.stop(3000));
		CHECK(readToEnd(restarted).empty());
		app.stopHttp();
		app.transport.verifyCaps();
		std::printf("MIXED_CAP_PASS cap=%zu\n", cap);
	}

	void fullStackRestart(bool managed, std::size_t initial_sse_clients)
	{
		Application app(4096, 4, managed, initial_sse_clients);
		sse::Token previous;
		sse::Frame frame;
		CHECK(sse::encode({"lifecycle", std::nullopt, std::nullopt, std::nullopt}, frame) == sse::EncodeResult::Ok);
		for(unsigned cycle = 0; cycle < 3; ++cycle)
		{
			if(cycle)
			{
				CHECK(app.transport.native.start().status == tcp::Status::Ok);
				app.start();
				until([&] {
					const auto result = app.events.publish(previous, frame);
					CHECK(result == sse::Publish::Closed || result == sse::Publish::Busy);
					return result == sse::Publish::Closed;
				});
			}

			CHECK(app.events.stats().memory.fixed_blocks == initial_sse_clients);
			auto peer = websocket(app.backend.port());
			auto events = eventStream(app);
			const auto token = app.token();
			if(cycle) CHECK(!(token == previous));

			const std::uint8_t payload[]{static_cast<std::uint8_t>(cycle), 0x12, 0x34};
			sendFrame(peer, 2, {payload, sizeof(payload)});
			CHECK(receiveFrame(peer).payload == std::vector<std::uint8_t>(std::begin(payload), std::end(payload)));
			publish(app, token, frame);
			CHECK(readUntil(events, "\n\n").find("lifecycle") != std::string::npos);
			CHECK(body(http(app.backend.port(), "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")) == "healthy");
			const auto tcp_before = app.tcp_allocations.bytes.load();
			const auto sse_before = app.sse_allocations.bytes.load();
			CHECK(tcp_before > 0 && sse_before > 0 && app.events.active() == 1 && app.endpoint.count() == 1);

			// No producers are in flight. A successful SSE stop must release even
			// configured fixed buffers; HTTP then joins its owner and disconnects WS.
			CHECK(app.events.stop(1000));
			CHECK(app.events.active() == 0 && app.events.stats().memory.allocated_bytes == 0);
			CHECK(app.sse_backend.bufferMemoryStats().used_bytes == 0);
			CHECK(app.sse_allocations.bytes == 0 && app.sse_allocations.blocks == 0);
			CHECK(readToEnd(events).empty());
			CHECK(app.server.end() == HttpError::Ok);
			CHECK(app.endpoint.count() == 0 && app.backend.bufferMemoryStats().used_bytes == 0);
			CHECK(app.tcp_allocations.bytes == 0 && app.tcp_allocations.blocks == 0);
			CHECK(app.transport.native.ownedHandles() == 0 && app.transport.activeAccepted() == 0);
			CHECK(readToEnd(peer).empty());
			CHECK(app.transport.native.stop().status == tcp::Status::Ok);
			CHECK(app.backend.diagnostics().state == TcpRuntimeState::Stopped);
			previous = token;
			std::printf(
				"TCP_FULL_RESTART managed=%u initial_sse=%zu cycle=%u tcp_payload=%zu sse_payload=%zu remaining=0\n",
				managed ? 1 : 0, initial_sse_clients, cycle, tcp_before, sse_before);
		}
	}

	void admissionCaps(unsigned endpoint_clients, unsigned admitted)
	{
		Application app(7, endpoint_clients);
		std::vector<Socket> peers;
		for(unsigned index = 0; index < admitted; ++index)
			peers.push_back(websocket(app.backend.port()));

		until([&] {
			return app.connections.load() == admitted && app.endpoint.count() == admitted;
		});
		const auto rejected = http(app.backend.port(), handshake_request);
		CHECK(rejected.find("503 ") != rejected.npos && rejected.find("101 ") == rejected.npos);
		CHECK(app.connections.load() == admitted && app.endpoint.count() == admitted);
		peers.clear();
		app.drained();
		auto recovered = websocket(app.backend.port());
		until([&] {
			return app.connections.load() == admitted + 1;
		});
		recovered.close();
		app.drained();
		std::printf("ADMISSION_CAP_PASS endpoint=%u admitted=%u\n", endpoint_clients, admitted);
	}

	void failedHandshake()
	{
		Application app(3);
		const std::string expected(handshake_response);
		const std::size_t cuts[]{0, 1, 45, expected.find("s3pPL") + 5, expected.size() - 1};
		unsigned connected = 0;
		for(const auto cut : cuts)
		{
			const auto faults = app.transport.faultCount();
			app.transport.failNextAcceptedWriteAfter(cut);
			auto peer = connectTo(app.backend.port());
			sendAll(peer, handshake_request);
			CHECK(shutdown(peer.value, sockets::shutdown_send) == 0);
			const auto actual = readToEnd(peer);
			CHECK(actual == expected.substr(0, cut));
			app.drained();
			CHECK(app.connections.load() == connected && app.endpoint.count() == 0);
			CHECK(app.transport.faultCount() == faults + 1 && app.transport.faultBytes() == cut);
			auto recovered = websocket(app.backend.port());
			++connected;
			until([&] {
				return app.connections.load() == connected;
			});
			recovered.close();
			app.drained();
			std::printf("PARTIAL_101_PASS accepted=%zu total=%zu\n", cut, expected.size());
		}
	}

} // namespace

int main()
{
	CHECK(sockets::initialize() == 0);
	for(const auto cap : {1u, 2u, 3u, 7u})
		mixedCaps(cap);

	admissionCaps(8, 4);
	admissionCaps(2, 2);
	failedHandshake();
	for(const bool managed : {false, true})
		for(const std::size_t initial_sse_clients : {0u, 1u})
			fullStackRestart(managed, initial_sse_clients);
	CHECK(sockets::cleanup() == 0);
	std::printf("TCP_TRANSPORT_INTEGRATION_PASS checks=%u\n", checks.load());
}
