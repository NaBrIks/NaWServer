// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/tcp.h>
#include <NaWServer/backends/tcp_sse.h>
#if defined(_WIN32)
#include <NaWServer/backends/windows_tcp.h>
#else
#include <NaWServer/backends/linux_tcp.h>
#endif
#include <NaWServer/http_server.h>
#include <NaWServer/websocket.h>
#include "../examples/common/sse_test.hpp"
#include "http_demo.h"
#include "mcp_demo.h"
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string_view>
#include <thread>

namespace
{

	volatile std::sig_atomic_t stopped = 0;

	void stop(int)
	{
		stopped = 1;
	}

}

int main(int argument_count, char **arguments)
{
	using namespace NaWServer;
	TcpOptions options;
	options.port = 8081;
	std::uint32_t duration_ms = 0;
	if(argument_count > 4)
		return 2;

	if(argument_count >= 2)
	{
		unsigned port = 0;
		const std::string_view value(arguments[1]);
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
		if(parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || port > 65535 || port == 443)
			return 2;

		options.port = static_cast<std::uint16_t>(port);
	}

	if(argument_count >= 3)
	{
		const std::string_view value(arguments[2]);
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), duration_ms);
		if(parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !duration_ms)
			return 2;
	}

	if(argument_count == 4)
		options.bind_address = arguments[3];

#if defined(_WIN32)
	tcp::WindowsTransport transport;
#else
	tcp::LinuxTransport transport;
#endif
	if(transport.start().status != tcp::Status::Ok)
		return 1;

	TcpHttpBackend backend(transport, options);
	demo::HttpRoutes http("config.tcp.json");
	if(!http.ready())
		return 1;

	sse::TcpAdapter sse_backend(backend);
	sse::EventSource events(sse_backend);
	examples::SseTest sse_test(events, http.chain);
	WebSocketEndpoint websocket("/ws");
	websocket.onMessage([](WebSocketClient *client, const uint8_t *data, std::size_t size, bool binary)
	{
		if(binary)
			(void)client->send_binary(data, size);
		else
		{
			const std::string text(reinterpret_cast<const char *>(data), size);
			(void)client->send(text.c_str());
		}
	});

	#if NAWSERVER_MCP
	demo::McpDemo mcp;
#endif
	HandlerBase *const routes[]{
#if NAWSERVER_MCP
		&mcp.endpoint,
#endif
		&http.mcp_client, &http.mcp_panel, &http.index, &http.stylesheet, &http.script, &http.profile, &http.health,
		&http.echo, &http.parameters, &http.private_route, &http.forbidden_route, &websocket,
		&sse_test.open_route, &sse_test.private_route, &sse_test.publish_route,
		&sse_test.close_route, &sse_test.stats_route, &sse_test.stop_route};
	HttpServer server(backend, routes);
	if(!events.start())
		return 1;

	if(server.begin() != HttpError::Ok)
	{
		std::fputs("Cannot start TCP HTTP/WS/SSE demo\n", stderr);
		while(!events.stop(5000))
			std::this_thread::sleep_for(std::chrono::milliseconds(20));

		return 1;
	}

	#if NAWSERVER_MCP
	mcp.bound_port(backend.port());
#endif
	std::signal(SIGINT, stop);
	std::signal(SIGTERM, stop);
	std::printf("NaWServer TCP HTTP/WS/SSE: http://%s:%u\n", options.bind_address, backend.port());
	std::fflush(stdout);
	const auto started = std::chrono::steady_clock::now();
	while(!stopped &&
	    (!duration_ms || std::chrono::steady_clock::now() - started < std::chrono::milliseconds(duration_ms)))
	{
		sse_test.poll();
		if(sse_test.stop_requested.exchange(false) && !events.stop(5000))
			std::fputs("SSE drain pending; retaining owners\n", stderr);

		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	while(!events.stop(5000))
		std::this_thread::sleep_for(std::chrono::milliseconds(20));

	while(server.end() != HttpError::Ok)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));

	return transport.stop().status == tcp::Status::Ok ? 0 : 1;
}
