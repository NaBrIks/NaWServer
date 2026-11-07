// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "support/host_sockets.h"
#include <NaWServer/backends/tcp.h>
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/http_server.h>
#include <NaWServer/handler_static.h>
#include <NaWServer/route.h>
#include <NaWServer/admission_http.h>
#include <NaWServer/sse.h>
#include <NaWServer/websocket.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace sockets = NaWServer::test::host;
using namespace NaWServer;
namespace
{
	void check(bool value, int line)
	{
		if(!value)
		{
			std::printf("TCP_QOS_STREAM_FAIL line=%d native=%d\n", line, sockets::lastError()); std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check(static_cast<bool>(value), __LINE__)
	struct Socket
	{
		sockets::Handle handle;
		Socket(const Socket &) = delete;
		explicit Socket(std::uint16_t port)
		  : handle(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))
		{
			CHECK(handle != sockets::invalid && sockets::setTimeouts(handle, 2000));
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = htons(port);
			CHECK(connect(handle, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
		}

		~Socket()
		{
			sockets::closeSocket(handle);
		}

		void send(const std::string &bytes)
		{
			std::size_t offset = 0;
			while(offset < bytes.size())
			{
				const int sent = sockets::sendSome(handle, bytes.data() + offset, bytes.size() - offset);
				CHECK(sent > 0);
				offset += static_cast<std::size_t>(sent);
			}
		}

		std::string until(const char *marker)
		{
			std::string bytes;
			char byte{};
			while(bytes.find(marker) == bytes.npos)
			{
				CHECK(bytes.size() < 4096 && sockets::receiveSome(handle, &byte, 1) == 1);
				bytes.push_back(byte);
			}
			return bytes;
		}

		std::string open(const char *path, const char *key, bool websocket)
		{
			send(std::string("GET ") + path + " HTTP/1.1\r\nHost: localhost\r\nX-Client-Key: " + key + "\r\n" +
			    (websocket ?
			    "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
			    : "") + "\r\n");
			return until("\r\n\r\n");
		}

		void ping()
		{
			// Masked ping containing 'x'; the original admitted stream must answer.
			send(std::string("\x89\x81\x01\x02\x03\x04\x79", 7));
			char frame[3];
			for(auto &byte : frame)
				CHECK(sockets::receiveSome(handle, &byte, 1) == 1);
			CHECK(static_cast<unsigned char>(frame[0]) == 0x8a && frame[1] == 1 && frame[2] == 'x');
		}

	};
}

int main()
{
	CHECK(sockets::initialize() == 0);
	sockets::TcpTransport transport;
	CHECK(transport.start().status == tcp::Status::Ok);
	AdmissionOptions configuration;
	configuration.reserved_key = "primary";
	configuration.reserved = {2, 1, 1, 0, 1000};
	configuration.shared = {2, 1, 1, 0, 1000};
	Admission<1> policy(configuration);
	auto now = +[]() -> std::uint64_t {
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	};
	AdmissionMiddleware middleware(AdmissionRef(policy), now);
	auto admission_chain = makeHeaderChain(middleware);
	TcpOptions options;
	options.port = 0;
	options.http_memory.enabled = true;
	options.managed.ingress_slots = 4;
	options.buffer_memory.max_bytes = 131072;
	TcpHttpBackend backend(transport, options);
	sse::TcpAdapter adapter(backend);
	sse::EventSource events(adapter);
	auto open = [&events](Request &request, Response &response)
		{
			const auto result = events.open(request, response);
			if(result.active || response.state() == ResponseState::External) return result.result;

			const bool busy = result.admission == sse::Admit::Full || result.admission == sse::Admit::NoMemory ||
			    result.admission == sse::Admit::Stopping;
			if(busy) (void)response.header("Retry-After", "1");

			return response.text(busy ? 503 : 400, "SSE admission rejected");
		};
	Route route("/events", HttpMethod::Get, EndpointRef(open));
	WebSocketEndpoint websocket("/ws");
	HandlerStaticUriText health("/health", "alive");
	HandlerBase *const handlers[]{&route, &websocket, &health};
	HttpLimits limits;
	limits.max_connections = 8;
	HttpServer server(backend, handlers, limits, {}, {}, admission_chain);
	CHECK(events.start() && server.begin() == HttpError::Ok);
	{
		Socket guest(backend.port()), primary(backend.port());
		CHECK(guest.open("/ws", "guest", true).find("HTTP/1.1 101") == 0);
		CHECK(primary.open("/ws", "primary", true).find("HTTP/1.1 101") == 0);
		const auto reserved = backend.bufferMemoryStats();
		for(unsigned index = 0; index < 10; ++index)
		{
			Socket excess(backend.port());
			CHECK(excess.open("/ws", "guest", true).find("HTTP/1.1 503") == 0);
			guest.ping();
			primary.ping();
		}
		CHECK(backend.bufferMemoryStats().used_bytes == reserved.used_bytes);
		Socket guest_events(backend.port()), primary_events(backend.port());
		CHECK(guest_events.open("/events", "guest", false).find("HTTP/1.1 200") == 0);
		CHECK(primary_events.open("/events", "primary", false).find("HTTP/1.1 200") == 0);
		Socket extra_events(backend.port());
		CHECK(extra_events.open("/events", "guest", false).find("HTTP/1.1 503") == 0);
		Socket rest(backend.port());
		CHECK(rest.open("/health", "primary", false).find("HTTP/1.1 200") == 0);
		CHECK(rest.until("alive").find("alive") != std::string::npos);
		guest.ping();
		primary.ping();
		CHECK(events.stop(2000));
	}
	CHECK(server.end() == HttpError::Ok);
	CHECK(backend.bufferMemoryStats().used_bytes == 0);
	CHECK(transport.ownedHandles() == 0 && transport.stop().status == tcp::Status::Ok);
	CHECK(sockets::cleanup() == 0);
	std::puts("TCP_QOS_STREAM_PASS");
}
