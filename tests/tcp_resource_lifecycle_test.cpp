// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include "support/host_sockets.h"
#include <NaWServer/backends/tcp.h>
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/sse.h>
#include <NaWServer/websocket.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace
{
	using namespace NaWServer;
	namespace sockets = NaWServer::test::host;
	void check(bool value, int line)
	{
		if(!value) { std::printf("RESOURCE_LIFECYCLE_FAIL %d native=%d\n", line, sockets::lastError()); std::fflush(stdout); std::_Exit(1); }
	}
#define CHECK(value) check(static_cast<bool>(value), __LINE__)
	template <class F> void until(F predicate)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while(!predicate())
		{
			CHECK(std::chrono::steady_clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	struct Policy
	{
		std::array<std::atomic<unsigned>, 3> active{};
		std::atomic<unsigned> released{0};
		std::thread::id owner;
		Result operator()(HeaderContext &context, HeaderNext &next)
		{
			if(owner == std::thread::id{}) owner = std::this_thread::get_id();
			CHECK(owner == std::this_thread::get_id()); ++active[0];
			context.request.requestState()->resource_lease = {this, {},
				[](void *raw, const RequestResourceLease::Token &token)
				{
					auto &policy = *static_cast<Policy *>(raw);
					CHECK(policy.owner == std::this_thread::get_id());
					CHECK(policy.active[token[0]].fetch_sub(1) > 0); ++policy.released;
				}, [](void *raw, RequestResourceLease::Token &token, RequestResource resource)
				{
					auto &policy = *static_cast<Policy *>(raw);
					CHECK(policy.owner == std::this_thread::get_id());
					const auto target = static_cast<std::size_t>(resource);
					if(target && policy.active[target] != 0) return false;
					CHECK(policy.active[token[0]].fetch_sub(1) > 0); ++policy.active[target]; token[0] = target;
					return true;
				}, false};
			return next();
		}
	};
	sockets::Handle open(std::uint16_t port, bool websocket)
	{
		const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		CHECK(socket != sockets::invalid && sockets::setTimeouts(socket, 3000));
		sockaddr_in address{}; address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
		CHECK(connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
		std::string head = websocket ? "GET /ws HTTP/1.1\r\nHost: test\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n" :
		    "GET /events HTTP/1.1\r\nHost: test\r\n\r\n";
		std::size_t offset = 0;
		while(offset < head.size())
		{
			const auto sent = sockets::sendSome(socket, head.data() + offset, head.size() - offset);
			CHECK(sent > 0); offset += static_cast<std::size_t>(sent);
		}
		return socket;
	}
	std::string head(sockets::Handle socket)
	{
		std::string value; char byte{};
		while(value.find("\r\n\r\n") == value.npos)
		{
			CHECK(value.size() < 4096 && sockets::receiveSome(socket, &byte, 1) == 1); value += byte;
		}
		return value;
	}
	void run(bool managed)
	{
		sockets::TcpTransport transport; CHECK(transport.start().status == tcp::Status::Ok);
		TcpOptions options; options.port = 0; options.http_memory.enabled = managed;
		options.wait_ceiling_ms = 2; options.close_timeout_ms = 10;
		options.buffer_memory.max_bytes = 192 * 1024;
		TcpHttpBackend backend(transport, options);
		sse::TcpAdapter adapter(backend); sse::EventSource events(adapter);
		sse::Token event_token; std::atomic<bool> opened{false};
		auto event = [&](Request &request, Response &response)
		{
			const auto result = events.open(request, response);
			if(result.active) { event_token = result.token; opened = true; return result.result; }
			return response.text(503, "Full");
		};
		Route event_route("/events", HttpMethod::Get, EndpointRef(event));
		WebSocketEndpoint ws("/ws"); HandlerBase *handlers[]{&event_route, &ws};
		Policy policy; auto chain = makeHeaderChain(policy);
		HttpServer server(backend, handlers, {}, {}, {}, chain);
		CHECK(events.start() && server.begin() == HttpError::Ok);
		auto socket = open(backend.port(), true); CHECK(head(socket).find("HTTP/1.1 101") == 0);
		until([&] { return policy.active[1] == 1; }); CHECK(policy.active[0] == 0 && policy.released == 0);
		auto excess = open(backend.port(), true); CHECK(head(excess).find("HTTP/1.1 503") == 0);
		sockets::closeSocket(excess); until([&] { return policy.released == 1; }); CHECK(policy.active[1] == 1);
		sockets::closeSocket(socket); until([&] { return policy.active[1] == 0 && policy.released == 2; });
		socket = open(backend.port(), false); CHECK(head(socket).find("HTTP/1.1 200") == 0);
		until([&] { return opened.load(); }); CHECK(policy.active[2] == 1 && policy.active[0] == 0);
		excess = open(backend.port(), false); CHECK(head(excess).find("HTTP/1.1 503") == 0);
		sockets::closeSocket(excess); until([&] { return policy.released == 3; }); CHECK(policy.active[2] == 1);
		sockets::closeSocket(socket); CHECK(events.close(event_token));
		until([&] { return policy.active[2] == 0 && policy.released == 4; });
		CHECK(events.stop(2000) && server.end() == HttpError::Ok);
		CHECK(transport.ownedHandles() == 0 && transport.stop().status == tcp::Status::Ok);
	}
}
int main()
{
	CHECK(sockets::initialize() == 0); run(false); run(true); CHECK(sockets::cleanup() == 0);
	std::puts("RESOURCE_LIFECYCLE_PASS managed/unmanaged WS/SSE HTTP-owner release-on-disconnect");
}
