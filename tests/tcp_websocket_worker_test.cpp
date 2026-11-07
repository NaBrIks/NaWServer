// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "support/host_sockets.h"
#include <NaWServer/backends/tcp.h>
#include <NaWServer/http_server.h>
#include <NaWServer/route.h>
#include <NaWServer/websocket.h>
#include <NaWServer/sse.h>
#include <NaWServer/backends/tcp_sse.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	using namespace NaWServer;
	using Clock = std::chrono::steady_clock;
	namespace sockets = NaWServer::test::host;
	std::atomic<unsigned> checks{0};

	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("TCP_WEBSOCKET_WORKER_FAIL line=%d native=%d\n", line, sockets::lastError());
			std::fflush(stdout);
			std::_Exit(1);
		}
	}

#define CHECK(value) check((value), __LINE__)

	template <class Predicate> void until(Predicate predicate)
	{
		const auto deadline = Clock::now() + std::chrono::seconds(5);
		while(!predicate())
		{
			CHECK(Clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	sockets::Handle connectTo(std::uint16_t port)
	{
		const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		CHECK(socket != sockets::invalid && sockets::setTimeouts(socket, 3000));
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);
		CHECK(connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
		return socket;
	}

	void sendAll(sockets::Handle socket, const std::string &bytes)
	{
		std::size_t offset = 0;
		while(offset < bytes.size())
		{
			const int sent = sockets::sendSome(socket, bytes.data() + offset, bytes.size() - offset);
			CHECK(sent > 0);
			offset += static_cast<std::size_t>(sent);
		}
	}

	std::string readHead(sockets::Handle socket)
	{
		std::string result;
		while(result.find("\r\n\r\n") == result.npos)
		{
			char byte{};
			CHECK(result.size() < 4096 && sockets::receiveSome(socket, &byte, 1) == 1);
			result.push_back(byte);
		}
		return result;
	}

	std::string frame(unsigned opcode, unsigned value)
	{
		// One-byte masked payload, deliberately easy to coalesce in one send.
		return {static_cast<char>(0x80 | opcode), static_cast<char>(0x81), 1, 2, 3, 4,
		    static_cast<char>(value ^ 1)};
	}

	void expectFrame(sockets::Handle socket, unsigned opcode, unsigned value)
	{
		std::array<unsigned char, 3> wire{};
		std::size_t offset = 0;
		while(offset < wire.size())
		{
			const int count = sockets::receiveSome(socket, reinterpret_cast<char *>(wire.data() + offset),
			    wire.size() - offset);
			CHECK(count > 0);
			offset += static_cast<std::size_t>(count);
		}
		CHECK(wire[0] == (0x80 | opcode) && wire[1] == 1 && wire[2] == value);
	}

	constexpr char upgrade[] = "GET /ws HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\n"
	    "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
	    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";

	struct CallbackGuard
	{
		std::atomic<unsigned> &active;
		explicit CallbackGuard(std::atomic<unsigned> &value) : active(value) { CHECK(active.fetch_add(1) == 0); }
		~CallbackGuard() { CHECK(active.fetch_sub(1) == 1); }
	};

	void coexistence(bool managed, bool shared = false)
	{
		sockets::TcpTransport transport;
		CHECK(transport.start().status == tcp::Status::Ok);
		TcpOptions options;
		options.port = 0;
		// Exercise the production 20 ms default and a lower-latency profile.
		if(managed) options.wait_ceiling_ms = 2;
		options.close_timeout_ms = 10;
		options.stop_timeout_ms = 100;
		options.http_memory.enabled = managed;
		options.transmission.enabled = shared;
		options.managed.stream_bytes_per_client = 20 * 1024;
		options.buffer_memory.max_bytes = 192 * 1024;
		TcpHttpBackend backend(transport, options);
		sse::TcpAdapter sse_adapter(backend);
		sse::EventSource events(sse_adapter);
		sse::Token event_token;
		std::atomic<bool> event_open{false};
		auto open_events = [&](Request &request, Response &response)
		{
			const auto opened = events.open(request, response);
			if(opened.active) { event_token = opened.token; event_open.store(true); }
			return opened.result;
		};
		Route event_route("/events", HttpMethod::Get, EndpointRef(open_events));
		auto fail_events = [&](Request &request, Response &response)
		{
			const auto opened = events.open(request, response);
			CHECK(opened.active);
			response.fail();
			return SendResult::TransportError;
		};
		Route failed_event_route("/events-failed", HttpMethod::Get, EndpointRef(fail_events));
		if(shared) CHECK(events.start());
		std::atomic<unsigned> callbacks{0}, connections{0}, messages{0};
		std::array<WebSocketClient *, 2> clients{};
		std::mutex mutex;
		std::condition_variable condition;
		std::atomic<bool> blocked{false};
		std::atomic<bool> file_enabled{true};
		bool released = false;
		auto blocking = [&](Response &response)
		{
			CallbackGuard guard(callbacks);
			const bool stream = shared && file_enabled.load();
			blocked.store(true);
			if(stream)
			{
				CHECK(response.beginStream(200, "application/octet-stream", 160 * 1024) == SendResult::Ok);
				std::array<std::uint8_t, 1024> bytes{};
				for(unsigned block = 0; block < 160; ++block)
				{
					bytes.fill(static_cast<std::uint8_t>(block));
					CHECK(response.write({bytes.data(), bytes.size()}) == SendResult::Ok);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}
			std::unique_lock<std::mutex> lock(mutex);
			CHECK(condition.wait_for(lock, std::chrono::seconds(10), [&] { return released; }));
			return stream ? response.finish() : response.empty();
		};
		Route route("/block", HttpMethod::Get, EndpointRef(blocking));
		WebSocketEndpoint endpoint("/ws");
		endpoint.onConnect([&](WebSocketClient *client)
		{
			CallbackGuard guard(callbacks);
			const auto index = connections.load();
			if(index < clients.size()) clients[index] = client;

			connections.fetch_add(1);
		});
		endpoint.onMessage([&](WebSocketClient *, const std::uint8_t *bytes, std::size_t size, bool)
		{
			CallbackGuard guard(callbacks);
			const auto index = messages.fetch_add(1);
			CHECK(size == 1 && bytes[0] == (index < 3 ? 100 + index : 110 + index - 3));
		});
		HandlerBase *const routes[]{&route, &endpoint, &event_route, &failed_event_route};
		HttpServer server(backend, routes);
		CHECK(server.begin() == HttpError::Ok);
		if(shared)
		{
			// Abort after SSE claim but before callbackComplete must cancel the
			// filling preface, never publish a stray HTTP200 header to the socket.
			const auto rejected_event = connectTo(backend.port());
			sendAll(rejected_event, "GET /events-failed HTTP/1.1\r\nHost: test\r\n\r\n");
			char byte{};
			CHECK(sockets::receiveSome(rejected_event, &byte, 1) == 0);
			until([&] { return events.active() == 0; });
			CHECK(sockets::closeSocket(rejected_event) == 0);
		}
		const auto first = connectTo(backend.port());
		const auto second = connectTo(backend.port());
		sendAll(first, upgrade);
		CHECK(readHead(first).find("HTTP/1.1 101") == 0);
		sendAll(second, upgrade);
		CHECK(readHead(second).find("HTTP/1.1 101") == 0);
		until([&] { return connections.load() == 2; });
		sockets::Handle event_socket = sockets::invalid;
		if(shared)
		{
			event_socket = connectTo(backend.port());
			sendAll(event_socket, "GET /events HTTP/1.1\r\nHost: test\r\n\r\n");
			CHECK(readHead(event_socket).find("HTTP/1.1 200") == 0);
			until([&] { return event_open.load(); });
			CHECK(sse_adapter.bufferMemoryStats().used_bytes == 0);
			CHECK(events.stats().memory.allocated_bytes == 0);
		}
		const auto http = connectTo(backend.port());
		sendAll(http, "GET /block HTTP/1.1\r\nHost: test\r\n\r\n");
		until([&] { return blocked.load(); });
		std::uint64_t maximum_round_ms = 0;
		for(unsigned sequence = 0; sequence < 12; ++sequence)
		{
			const auto started = Clock::now();
			for(auto *client : clients)
			{
				const auto payload = static_cast<std::uint8_t>(sequence);
				until([&]
				{
					const auto result = shared ? client->send_binary(&payload, 1, {1, 0, false}) :
					    client->send_binary(&payload, 1);
					CHECK(result == WebSocketSendResult::Queued || result == WebSocketSendResult::Busy ||
					    result == WebSocketSendResult::GlobalFull);
					return result == WebSocketSendResult::Queued;
				});
			}
			expectFrame(first, 2, sequence);
			expectFrame(second, 2, sequence);
			sendAll(first, frame(9, sequence));
			sendAll(second, frame(9, sequence));
			expectFrame(first, 10, sequence);
			expectFrame(second, 10, sequence);
			CHECK(callbacks.load() == 1 && messages.load() == 0);
			if(shared)
			{
				sse::Frame event;
				CHECK(sse::encode({"shared", std::nullopt, std::nullopt, std::nullopt}, event) == sse::EncodeResult::Ok);
				until([&] { return events.publish(event_token, event) == sse::Publish::Queued; });
				std::string received(event.size, '\0');
				std::size_t offset = 0;
				while(offset < event.size)
				{
					const int count = sockets::receiveSome(event_socket, received.data() + offset, event.size - offset);
					CHECK(count > 0);
					offset += static_cast<std::size_t>(count);
				}
				CHECK(received == std::string(event.bytes.data(), event.size));
			}
			const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			    Clock::now() - started).count());
			if(elapsed > maximum_round_ms) maximum_round_ms = elapsed;
		}
		// First message occupies the callback mailbox, second stages in decoder
		// storage, third remains in TCP. No loss/close on normal scheduling delay.
		sendAll(first, frame(2, 100) + frame(2, 101) + frame(2, 102));
		std::this_thread::sleep_for(std::chrono::milliseconds(80));
		CHECK(messages.load() == 0 && endpoint.count() == 2);
		sendAll(second, frame(9, 77));
		expectFrame(second, 10, 77);
		{
			std::lock_guard<std::mutex> lock(mutex);
			released = true;
		}
		condition.notify_one();
		CHECK(readHead(http).find(shared ? "HTTP/1.1 200" : "HTTP/1.1 204") == 0);
		if(shared)
		{
			std::size_t received = 0;
			std::array<std::uint8_t, 4096> bytes{};
			while(received < 160 * 1024)
			{
				const int count = sockets::receiveSome(http, reinterpret_cast<char *>(bytes.data()), bytes.size());
				CHECK(count > 0);
				for(int index = 0; index < count; ++index)
					CHECK(bytes[static_cast<std::size_t>(index)] == ((received + static_cast<std::size_t>(index)) / 1024) % 256);
				received += static_cast<std::size_t>(count);
			}
		}
		CHECK(sockets::closeSocket(http) == 0);
		until([&] { return messages.load() == 3; });
		sendAll(first, frame(9, 103));
		expectFrame(first, 10, 103);
		CHECK(sockets::closeSocket(first) == 0 && sockets::closeSocket(second) == 0);
		until([&] { return endpoint.count() == 0; });
		// Prefix may contain multiple frames. All callbacks still execute on the
		// HTTP owner before publishing the WS socket to its dedicated worker.
		const auto prefixed = connectTo(backend.port());
		sendAll(prefixed, std::string(upgrade) + frame(2, 110) + frame(2, 111) + frame(2, 112));
		CHECK(readHead(prefixed).find("HTTP/1.1 101") == 0);
		until([&] { return messages.load() == 6; });
		sendAll(prefixed, frame(9, 113));
		expectFrame(prefixed, 10, 113);

		CHECK(sockets::closeSocket(prefixed) == 0);
		until([&] { return endpoint.count() == 0; });
		if(shared)
		{
			CHECK(events.active() == 1);
			until([&] { return events.close(event_token); });
			until([&] { return events.active() == 0; });
			CHECK(events.stop(1000) && sockets::closeSocket(event_socket) == 0);
			CHECK(backend.transmissionStats().sent_bytes > 0);
		}
		CHECK(server.end() == HttpError::Ok && transport.ownedHandles() == 0);
		CHECK(server.begin() == HttpError::Ok);
		const auto stopping_ws = connectTo(backend.port());
		sendAll(stopping_ws, upgrade);
		CHECK(readHead(stopping_ws).find("HTTP/1.1 101") == 0);
		sendAll(stopping_ws, frame(9, 120));
		expectFrame(stopping_ws, 10, 120);
		// A blocked callback causes bounded stop failure. Both workers and their
		// borrowed state survive until the callback returns and stop is retried.
		blocked.store(false);
		file_enabled.store(false);
		{
			std::lock_guard<std::mutex> lock(mutex);
			released = false;
		}
		const auto stopping_http = connectTo(backend.port());
		sendAll(stopping_http, "GET /block HTTP/1.1\r\nHost: test\r\n\r\n");
		until([&] { return blocked.load(); });
		CHECK(server.end() == HttpError::CleanupRequired);
		CHECK(endpoint.count() == 1);
		{
			std::lock_guard<std::mutex> lock(mutex);
			released = true;
		}
		condition.notify_one();
		until([&] { return callbacks.load() == 0; });
		CHECK(server.end() == HttpError::Ok);
		CHECK(endpoint.count() == 0 && sockets::closeSocket(stopping_ws) == 0);
		CHECK(sockets::closeSocket(stopping_http) == 0);
		CHECK(transport.ownedHandles() == 0 && transport.stop().status == tcp::Status::Ok);
		std::printf("WS_WORKER_COEXISTENCE_PASS managed=%u wait_ms=%u max_round_ms=%llu "
		    "outgoing=24 pong=25 deferred=3 prefix=3\n", managed, options.wait_ceiling_ms,
		    static_cast<unsigned long long>(maximum_round_ms));
	}

#if NAWSERVER_TCP_MANAGED
	void smallArenaRejectsBeforeUpgrade()
	{
		sockets::TcpTransport transport;
		CHECK(transport.start().status == tcp::Status::Ok);
		TcpOptions options;
		options.port = 0;
		options.http_memory.enabled = true;
		options.buffer_memory.max_bytes = 192 * 1024;
		options.managed.stream_bytes_per_client = 12 * 1024;
		TcpHttpBackend backend(transport, options);
		WebSocketEndpoint endpoint("/ws");
		HandlerBase *const routes[]{&endpoint};
		HttpServer server(backend, routes);
		CHECK(server.begin() == HttpError::Ok);
		const auto client = connectTo(backend.port());
		sendAll(client, upgrade);
		CHECK(readHead(client).find("HTTP/1.1 503") == 0);
		CHECK(endpoint.count() == 0);
		CHECK(sockets::closeSocket(client) == 0);
		CHECK(server.end() == HttpError::Ok && transport.ownedHandles() == 0);
		CHECK(backend.bufferMemoryStats().used_bytes == 0);
		CHECK(transport.stop().status == tcp::Status::Ok);
	}
#endif
}

int main()
{
	CHECK(sockets::initialize() == 0);
	coexistence(false);
	coexistence(false, true);
#if NAWSERVER_TCP_MANAGED
	coexistence(true);
	coexistence(true, true);
	smallArenaRejectsBeforeUpgrade();
#endif
	CHECK(sockets::cleanup() == 0);
	std::printf("TCP_WEBSOCKET_WORKER_PASS checks=%u\n", checks.load());
}
