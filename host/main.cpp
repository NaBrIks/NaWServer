// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/http_server.h>
#include <NaWServer/backends/windows.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/handler_static.h>
#include <NaWServer/handler_middleware.h>
#include <NaWServer/websocket.h>
#include <NaWServer/backends/windows_sse.h>
#include "../examples/common/sse_test.hpp"
#include "http_demo.h"
#include "mcp_demo.h"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <thread>
#include <chrono>
#include <fstream>
#include <iterator>

using namespace NaWServer;
namespace
{
	volatile std::sig_atomic_t stopped = 0;
	void stop(int)
	{
		stopped = 1;
	}

}

int main()
{
	WindowsHttpBackend backend(8080);
	demo::HttpRoutes http("config.windows.json");
	if(!http.ready())
	{
		std::fputs("Cannot read HTTP test frontend assets\n", stderr); return 1;
	}

	sse::WindowsAdapter sse_backend;
	sse::EventSource events(sse_backend);
	examples::SseTest sse_test(events, http.chain);
	WebSocketEndpoint websocket("/ws");
	websocket.onMessage([](WebSocketClient *client, const uint8_t *data, std::size_t size, bool binary)
	{
		if(binary) (void)client->send_binary(data, size);
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
	if(!events.start()) { std::fputs("Cannot start SSE\n", stderr); return 1; }

	if(server.begin() != HttpError::Ok)
	{
		std::fputs("Cannot start loopback HTTP on port 8080\n", stderr);
		while(!events.stop(5000))
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		return 1;
	}

	#if NAWSERVER_MCP
	mcp.bound_port(backend.port());
#endif
	std::signal(SIGINT, stop);
	std::signal(SIGTERM, stop);
	std::printf("NaWServer: http://127.0.0.1:%u (Ctrl+C to stop)\n", backend.port());
	std::fflush(stdout);
	while(!stopped)
	{
		sse_test.poll();
		if(sse_test.stop_requested.exchange(false) && !events.stop(5000))
			std::fputs("SSE stop timed out; ownership retained\n", stderr);

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	while(!events.stop(5000))
	{
		std::fputs("SSE drain pending; retaining backend for retry\n", stderr);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	while(server.end() != HttpError::Ok)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	return 0;
}
