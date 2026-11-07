// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/http_server.h>
#include <NaWServer/handler_middleware.h>
#include <NaWServer/route.h>
#include <NaWServer/websocket.h>
#include <NaWServer/handler_static.h>
#include <NaWServer/sse.h>
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/backends/esp_idf_tcp.h>
#include <NaWServer/backends/tcp.h>
#include "esp_log.h"

using namespace NaWServer;
static Result private_endpoint(Context &ctx)
{
	(void)ctx.response.text(200, "authorized");
	return Result::Complete;
}

extern "C" void app_main()
{
	FixedBasicAuthenticator users("test", "test-only", 3);
	AuthMiddleware auth(users);
	AccessMiddleware access;
	HeaderChain<AuthMiddleware, AccessMiddleware> typed(auth, access);
	auto factory = makeHeaderChain(auth, access);
	HeaderFixedChain<2> fixed(headerMiddlewareRef(auth), headerMiddlewareRef(access));
	Route one("/typed", HttpMethod::Get, private_endpoint, {}, AccessPolicy::require(1), {}, typed);
	Route two("/factory", HttpMethod::Get, private_endpoint, {}, AccessPolicy::authenticated(), {}, factory);
	Route three("/fixed", HttpMethod::Get, private_endpoint, {}, AccessPolicy::authenticated(), {}, fixed);
	HandlerStaticUriText page("/", "compile check");
	WebSocketEndpoint ws("/ws");
	HandlerBase *const routes[] = {&page, &one, &two, &three, &ws};
	tcp::IdfTransport transport;
	TcpHttpBackend backend(transport);
	HttpServer server(backend, routes);
	sse::TcpAdapter events_backend(backend);
	sse::EventSource events(events_backend);
	auto sse_endpoint = [&](Request &request, Response &response)
		{
			return events.open(request, response).result;
		};
	Route event_route("/events", HttpMethod::Get, EndpointRef(sse_endpoint), {}, AccessPolicy::require(1), {}, typed);
	(void)event_route;
	if(esp_log_level_get("compile_check") == ESP_LOG_VERBOSE)
	{
		(void)transport.start();
		server.begin();
		(void)ws.send("compile check");
		(void)ws.send_binary(nullptr, 0);
		if(events.start())
			(void)events.stop(1000);

		server.end();
	}
}
