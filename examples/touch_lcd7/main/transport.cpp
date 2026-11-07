// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "sdkconfig.h"
#include "../../common/demo_mcp.hpp"
#include "../../common/demo_admission.hpp"
#include <NaWServer/http_server.h>
#include <NaWServer/backends/esp_idf_tcp.h>
#include <NaWServer/backends/tcp.h>
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/handler.h>
#include "application.hpp"
namespace touch_lcd7
{
	NaWServer::HandlerBase *status_handler();
	NaWServer::HandlerBase *control_handler();
} // namespace touch_lcd7
#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <NaWServer/handler_middleware.h>
#include <NaWServer/route.h>
#include <NaWServer/websocket.h>
#include <NaWServer/sse.h>
#include "assets.hpp"
#include "telemetry.hpp"
#include "network.hpp"
using namespace NaWServer;
using namespace lcd7_web;
namespace
{
	// HTTP and WS callbacks share one I/O owner; the producer has its own buffer.
	uint8_t http_buffer[frame_limit], echo_payload[frame_limit], producer_buffer[frame_limit];
	tcp::IdfTransport tcp_transport;
	TcpOptions tcp_options()
	{
		TcpOptions options;
		options.bind_address = "0.0.0.0";
		options.port = 80;
		nawserver_demo_admission::configure(options);
		options.max_websocket_clients = network_policy.websocket_clients;
		return options;
	}

	TcpHttpBackend http_backend(tcp_transport, tcp_options());
	void collect_connections();
	class DeviceSocket : public WebSocketEndpoint
	{
		WebSocketExchange *callback_exchange_ = nullptr;
	public:
		DeviceSocket()
		  : WebSocketEndpoint("/ws")
		{
		}

		void handleRequest(Request *request, Response &response) override
		{
			collect_connections();
			callback_exchange_ = static_cast<WebSocketExchange *>(
				request->transportExtension(&websocket_exchange_key));
			if(callback_exchange_ && callback_exchange_->opening())
			{
				if(const auto *reason = network_admission("ws"))
				{
					network_rejected(reason);
					closeCallback();
					callback_exchange_ = nullptr;
					return;
				}
			}

			WebSocketEndpoint::handleRequest(request, response);
			callback_exchange_ = nullptr;
			collect_connections();
		}

		void closeCallback()
		{
			if(callback_exchange_)
				callback_exchange_->close();
		}

	} websocket;
	FixedBasicAuthenticator fixture("demo", "showcase-only", 1);
	AuthMiddleware auth(fixture);
	AccessMiddleware access;
	auto protected_chain = makeHeaderChain(auth, access);
#if NAWSERVER_MCP
	using McpExperiments = nawserver_demo_mcp::ExperimentTools<ExperimentCommand, runtime_state, set_experiment, 1>;
	// Discovery and read-only calls share the existing command API credentials.
	auto mcp_handler = [](Context &context)
		{
			nawserver_demo_mcp::endpoint<McpExperiments>.handleRequest(&context.request, context.response);
			return Result::Complete;
		};
	Route mcp_route("/mcp", Route::any_method, EndpointRef(mcp_handler), {}, AccessPolicy::authenticated(), {}, protected_chain);
#endif
	static_assert(network_policy.sse_clients <= sse::client_capacity);
	sse::TcpAdapter diagnostic_backend(http_backend);
	sse::EventSource diagnostic_events(diagnostic_backend);
	void collect_connections()
	{
		// The transport counts its listener too. No SDK session enumeration is used.
		const auto owned = tcp_transport.ownedHandles();
		network_collect(owned ? owned - 1 : 0, websocket.count(), diagnostic_events.active());
	}


	std::atomic<bool> controls_changed{false};
	std::array<sse::Token, sse::client_capacity> event_clients{};
	portMUX_TYPE event_clients_lock = portMUX_INITIALIZER_UNLOCKED;
	SendResult subscribe_events(Request &request, Response &response)
	{
		collect_connections();
		if(const auto *reason = network_admission("sse"))
			return network_busy(request, response, reason);

		const auto opened = diagnostic_events.open(request, response);
		if(opened.result == SendResult::Ok)
		{
			portENTER_CRITICAL(&event_clients_lock);
			event_clients[opened.token.slot] = opened.token;
			portEXIT_CRITICAL(&event_clients_lock);
			return SendResult::Ok;
		}

		if(response.state() == ResponseState::External)
			return opened.result;

		if(opened.admission == sse::Admit::Full || opened.admission == sse::Admit::NoMemory ||
		    opened.admission == sse::Admit::Stopping)
			return network_busy(request, response, "sse_limit");

		return response.text(400, "SSE admission rejected");
	}

	Route events_route("/api/v2/events", HttpMethod::Get, subscribe_events);
	Route private_events_route(
		"/api/v2/events/private",
		HttpMethod::Get,
		subscribe_events, {}, AccessPolicy::require(1), {}, protected_chain);
	SendResult auth_status(Response &response)
	{
		return response.text(200, "Authenticated: demo; permission mask: 1");
	}

	Route auth_route("/api/v2/auth/status", HttpMethod::Get, auth_status, {}, AccessPolicy::require(1), {}, protected_chain);
	Route admin_route("/api/v2/auth/admin", HttpMethod::Get, auth_status, {}, AccessPolicy::require(2), {}, protected_chain);

	Result finish(Response &response, Writer &writer, int status = 200)
	{
		const size_t size = writer.size(http_buffer);
		if(!size)
			return responseResult(response.text(500, "CBOR output limit"));

		return responseResult(response.send(status, "application/cbor", {http_buffer, size}));
	}

	Result error(Response &response, int status, const char *code, const char *message = nullptr)
	{
		count_rejected();
		Writer w(http_buffer, sizeof(http_buffer));
		w.map(2);
		w.put("v", 2);
		w.key("error");
		w.map(message ? 2 : 1);
		w.put("code", code);
		if(message)
			w.put("message", message);

		w.end();
		w.end();
		return finish(response, w, status);
	}

	bool content_type(Request &r, const char *expected)
	{
		auto value = r.header("Content-Type").value_or("");
		auto semicolon = value.find(';');
		auto type = value.substr(0, semicolon);
		while(!type.empty() && type.back() == ' ')
			type.pop_back();
		return type == expected;
	}

	bool origin_allowed(Request &r)
	{
		auto origin = r.header("Origin");
		return !origin || *origin == "http://" + r.host();
	}

	Result command(Context &context)
	{
		auto &request = context.request;
		auto &response = context.response;
		if(!origin_allowed(request))
			return error(response, 403, "origin_rejected");

		if(!content_type(request, "application/cbor"))
			return error(response, 415, "unsupported_media_type");

		const ByteView body = request.body();
		Fields fields;
		// Parse the borrowed bytes synchronously, then pass only bounded typed values to workers.
		// No body view, request pointer, CborValue, or borrowed string crosses the callback boundary.
		if(!fields.parse(body.data(), body.size()) || !fields.version())
			return error(response, 400, "invalid_payload");

		Writer w(http_buffer, sizeof(http_buffer));
		const auto path = request.path();
		if(path == "/api/v2/control")
		{
			char profile[20];
			if(!fields.string("profile", profile, sizeof(profile)) ||
			    (std::strcmp(profile, "quiet") && std::strcmp(profile, "balanced") &&
			    std::strcmp(profile, "performance")))
				return error(response, 400, "invalid_argument");

			w.map(4);
			w.put("v", 2);
			w.put("profile", profile);
			w.put("storage", "RAM only");
			w.put("effect", "demo only");
			w.end();
			return finish(response, w);
		}

		if(path == "/api/v2/experiments")
		{
			char action[12], kind[12] = "pi";
			ExperimentCommand c;
			if(!fields.string("action", action, sizeof(action)))
				return error(response, 400, "invalid_argument");

			c.stop = std::strcmp(action, "stop") == 0;
			if(!c.stop && std::strcmp(action, "start"))
				return error(response, 400, "invalid_argument");

			if(fields.find("kind") && !fields.string("kind", kind, sizeof(kind)))
				return error(response, 400, "invalid_argument");

			if(std::strcmp(kind, "pi") && std::strcmp(kind, "pulse"))
				return error(response, 400, "invalid_argument");

			c.pulse = std::strcmp(kind, "pulse") == 0;
			if(!fields.integer("duration_ms", c.duration, 5000) || !fields.integer("on_ms", c.on, 200) ||
			    !fields.integer("period_ms", c.period, 1000) || !fields.integer("core", c.core, 0) ||
			    !fields.integer("priority", c.priority, 2) || !set_experiment(c))
				return error(response, 400, "invalid_argument");

			encode_experiment(w, runtime_state().experiment);
			return finish(response, w);
		}

		bool enabled = false;
		if(!fields.boolean("enabled", enabled))
			return error(response, 400, "invalid_argument");

		int period = 1000;
		if(!fields.integer("period_ms", period, 1000))
			return error(response, 400, "invalid_argument");

		if(path == "/api/v2/led")
		{
			if(!runtime_state().led.supported)
				return error(response, 501, "unsupported", "LED driver is not assigned on this LCD board");

			int duty = 50;
			if(!fields.integer("duty_pct", duty, 50) || !set_led(enabled, period, duty))
				return error(response, 400, "invalid_argument");

			controls_changed.store(true);
			encode_led(w, runtime_state().led);
			return finish(response, w);
		}

		char mode[12];
		int color = 0xffa030, brightness = 20;
		if(!fields.string("mode", mode, sizeof(mode)) || !fields.integer("color", color, 0xffa030) ||
		    !fields.integer("brightness_pct", brightness, 20))
			return error(response, 400, "invalid_argument");

		int effect = std::strcmp(mode, "solid") == 0 ? 0
		             : std::strcmp(mode, "chase") == 0 ? 1
		             : std::strcmp(mode, "rainbow") == 0 ? 2
		                                                 : -1;
		int driver = -1;
		char driver_name[8];
		if(fields.find("driver"))
		{
			if(!fields.string("driver", driver_name, sizeof(driver_name)))
				return error(response, 400, "invalid_argument");

			driver = std::strcmp(driver_name, "rmt") == 0 ? 0 : std::strcmp(driver_name, "spi") == 0 ? 1 : -1;
			if(driver < 0)
				return error(response, 400, "invalid_argument");
		}

		if(!set_strip(enabled, effect, color, brightness, period, driver))
			return error(response, 400, "invalid_argument");

		controls_changed.store(true);
		encode_strip(w, runtime_state().strip);
		return finish(response, w);
	}

	Route experiment_route("/api/v2/experiments", HttpMethod::Post, command, {}, AccessPolicy::authenticated(), {}, protected_chain);
	Route led_route("/api/v2/led", HttpMethod::Post, command, {}, AccessPolicy::authenticated(), {}, protected_chain);
	Route strip_route("/api/v2/strip", HttpMethod::Post, command, {}, AccessPolicy::authenticated(), {}, protected_chain);
	Route control_route("/api/v2/control", HttpMethod::Post, command, {}, AccessPolicy::authenticated(), {}, protected_chain);
	Result connections(Context &ctx)
	{
		return error(ctx.response, 501, "unsupported", "Session detail API is unavailable in this core revision");
	}

	Route
	    connection_route("/api/v2/connections", HttpMethod::Get, connections, {}, AccessPolicy::authenticated(), {}, protected_chain);
	class Assets : public HandlerBase
	{
	public:
		RouteDescriptors routes() const override
		{
			static const auto descriptors = []
			{
				std::array<RouteDescriptor, sizeof(lcd7_assets::files) / sizeof(lcd7_assets::files[0])> values{};
				for(std::size_t i = 0; i < values.size(); ++i)
					values[i] = {lcd7_assets::files[i].path, HttpMethod::Get};
				return values;
			}();
			return {descriptors.data(), descriptors.size()};
		}

		void handleRequest(Request *request, Response &response) override
		{
			if(request->method() != HttpMethod::Get)
			{
				error(response, 405, "method_not_allowed");
				return;
			}

			for(const auto &asset : lcd7_assets::files)
			{
				if(request->path() != asset.path)
					continue;

				(void)response.header("Content-Encoding", "gzip");
				(void)response.header("Vary", "Accept-Encoding");
				(void)response.sendImmutable(200, asset.mime, {asset.data, asset.length});
				return;
			}
		}

	} assets;
	bool query_number(Request &r, const char *key, int &out, int fallback)
	{
		std::string value(r.parameter(key).value_or(std::string_view{}));
		if(value.empty())
		{
			out = fallback;
			return true;
		}

		if(value.size() > 9 || value.find('\0') != std::string::npos)
			return false;

		char *end = nullptr;
		errno = 0;
		long n = std::strtol(value.c_str(), &end, 10);
		if(errno || !end || *end || n < 0 || n > 64)
			return false;

		out = static_cast<int>(n);
		return true;
	}

	class PublicRoutes : public HandlerBase
	{
	public:
		RouteDescriptors routes() const override
		{
			static constexpr RouteDescriptor descriptors[] = {
				{"/api/v2/network", HttpMethod::Get},
				{"/api/v2/admission", HttpMethod::Get},
				{"/health", HttpMethod::Get},
				{"/go", HttpMethod::Get},
				{"/api/v2/info", HttpMethod::Get},
				{"/api/v2/summary", HttpMethod::Get},
				{"/api/v2/tasks", HttpMethod::Get},
				{"/api/v2/experiments", HttpMethod::Get},
				{"/api/v2/led", HttpMethod::Get},
				{"/api/v2/strip", HttpMethod::Get},
				{"/api/v2/echo", HttpMethod::Get},
				{"/api/v2/stream", HttpMethod::Get},
				{"/api/v2/memory", HttpMethod::Get},
				{"/api/v2/echo", HttpMethod::Post},
			};
			return {descriptors, sizeof(descriptors) / sizeof(descriptors[0])};
		}

		void handleRequest(Request *r, Response &response) override
		{
			const auto path = r->path();
			if(path == "/go")
			{
				(void)response.redirect("/#experiments");
				return;
			}

			if(path == "/api/v2/echo")
			{
				if(r->method() == HttpMethod::Post && !content_type(*r, "application/x-www-form-urlencoded"))
				{
					error(response, 415, "unsupported_media_type");
					return;
				}

				(void)response.text(200, r->parameter("value").value_or(std::string_view{}));
				return;
			}

			if(path == "/api/v2/stream")
			{
				TextBuffer<128> text;
				text.print("headers → buffered content → response. Not SSE.");
				(void)response.text(200, text.view());
				return;
			}

			Writer w(http_buffer, sizeof(http_buffer));
			if(path == "/api/v2/network")
			{
				const bool busy = network_admission("http") != nullptr;
				if(busy)
					(void)response.options(
						{ContentType::Default, CachePolicy::NoStore, HeaderFlags::NoSniff, ConnectionPolicy::Close});

				encode_network(w);
				finish(response, w);

				return;
			}

			if(path == "/api/v2/admission")
			{
				auto kind = r->parameter("kind").value_or("ws");
				if(kind != "ws" && kind != "sse")
				{
					error(response, 400, "invalid_argument");
					return;
				}

				const auto *reason = network_admission(kind == "ws" ? "ws" : "sse");
				if(reason)
				{
					(void)network_busy(*r, response, reason);
					return;
				}

				w.map(3);
				w.put("ok", true);
				w.put("retry_after_ms", network_policy.retry_after_ms);
				w.key("network");
				encode_network(w);
				w.end();
				finish(response, w);
				return;
			}

			if(path == "/health")
			{
				w.map(3);
				w.put("v", 2);
				w.put("ok", true);
				w.put("source", "device");
				w.end();
			}
			else if(path == "/api/v2/info")
				encode_info(w);

			else if(path == "/api/v2/memory")
				nawserver_demo_admission::encode_memory(w, http_backend, diagnostic_backend);
			else if(path == "/api/v2/summary")
				encode_summary(w, websocket.count(), false);
			else if(path == "/api/v2/experiments")
				encode_experiment(w, runtime_state().experiment);
			else if(path == "/api/v2/led")
				encode_led(w, runtime_state().led);
			else if(path == "/api/v2/strip")
				encode_strip(w, runtime_state().strip);
			else
			{
				int offset, limit;
				if(!query_number(*r, "offset", offset, 0) || !query_number(*r, "limit", limit, 8) || limit < 1 ||
				    limit > 8)
				{
					error(response, 400, "invalid_argument");
					return;
				}

				std::string snapshot(r->parameter("snapshot_id").value_or(std::string_view{}));
				if(snapshot.find('\0') != std::string::npos || !valid_snapshot(snapshot.c_str()))
				{
					error(response, 409, "snapshot_changed");
					return;
				}

				encode_tasks(w, offset, limit);
			}

			finish(response, w);
		}

	} public_routes;
	struct HttpCounter
	{
		Result operator()(HeaderContext &context, HeaderNext &next) const
		{
			count_http();
			collect_connections();
			if(context.request.path() != "/api/v2/network" && context.request.path() != "/api/v2/admission" &&
				network_admission("http") != nullptr)
				return responseResult(network_busy(context.request, context.response, "http_reserve"));
			return next();
		}
	} http_counter;
	auto http_counter_chain = nawserver_demo_admission::makeHeaderChain(http_counter);
	HandlerBase *const route_table[] = {
#if NAWSERVER_MCP
		&mcp_route,
#endif
		touch_lcd7::status_handler(),
		touch_lcd7::control_handler(),
		&assets,
		&public_routes,
		&experiment_route,
		&led_route,
		&strip_route,
		&control_route,
		&connection_route,
		&events_route,
		&private_events_route,
		&auth_route,
		&admin_route,
		&websocket};
	inline constexpr Header commonHeaders[]{
		{"X-Showcase-Source", "device"},
		{"Content-Security-Policy",
		 "default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; script-src 'self'; connect-src "
		 "'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'"}};
	inline constexpr DefaultHeaders defaultHeaders{commonHeaders};
	inline constexpr ResponseOptions responseOptions{ContentType::Default, CachePolicy::NoStore, HeaderFlags::NoSniff};
	// Budget WS/SSE, browser HTTP keep-alive pools and overlapping reloads separately.
	HttpLimits http_limits()
	{
		HttpLimits limits;
		// The powered LCD also needs DMA RAM; 8 KiB of HTTP headers exhausted
		// internal heap during browser reloads. The native profile allowed 1 KiB.
		limits.max_header_bytes = 2048;
		limits.max_connections = network_policy.httpd_connections;
		nawserver_demo_admission::configure(limits);
		return limits;
	}

	HttpServer server(http_backend, route_table, http_limits(), defaultHeaders, responseOptions);
	void ws_error(WebSocketClient *client, const char *code)
	{
		count_rejected();
		Writer w(http_buffer, sizeof(http_buffer));
		w.map(3);
		w.put("v", 2);
		w.put("type", "error");
		w.key("error");
		w.map(1);
		w.put("code", code);
		w.end();
		w.end();
		auto size = w.size(http_buffer);
		if(size)
		{
			if(client->send_binary(http_buffer, size) == WebSocketSendResult::Queued)
				count_tx(size, 1);
			else
				count_rejected();
		}
	}

	void ws_message(WebSocketClient *client, const uint8_t *data, size_t size, bool binary)
	{
		count_rx(size);
		if(!binary || size > frame_limit)
		{
			count_rejected();
			websocket.closeCallback();

			return;
		}

		Fields root, payload;
		char type[20], id[33];
		if(!root.parse(data, size) || !root.version() || !root.string("type", type, sizeof(type)) ||
		    std::strcmp(type, "echo.request") || !root.string("request_id", id, sizeof(id)) ||
		    !root.child("data", payload))
		{
			ws_error(client, "invalid_message");
			return;
		}

		bool bytes = payload.find("bytes"), text = payload.find("text");
		size_t length = sizeof(echo_payload);
		if(bytes == text || !payload.payload(bytes ? "bytes" : "text", echo_payload, length, bytes))
		{
			ws_error(client, "invalid_message");
			return;
		}

		Writer w(http_buffer, sizeof(http_buffer));
		w.map(4);
		w.put("v", 2);
		w.put("type", "echo.response");
		w.put("request_id", id);
		w.key("data");
		w.map(1);
		w.key(bytes ? "bytes" : "text");
		if(bytes)
			w.bytes(echo_payload, length);
		else
			w.text(reinterpret_cast<const char *>(echo_payload), length);

		w.end();
		w.end();
		auto result = w.size(http_buffer);
		if(!result)
		{
			ws_error(client, "invalid_message");
			return;
		}

		if(client->send_binary(http_buffer, result) == WebSocketSendResult::Queued)
			count_tx(result, 1);
		else
			count_rejected();
	}

	void producer(void *)
	{
		TickType_t wake = xTaskGetTickCount();
		unsigned tick = 0;
		for(;;)
		{
			vTaskDelayUntil(&wake, pdMS_TO_TICKS(100));
			telemetry_collect(++tick % 10 == 0);
			if(tick % 10 == 0)
			{
				collect_connections();

				char text[384], id[80];
				const auto size = diagnostic_json(text, sizeof(text), id, sizeof(id));
				sse::Frame frame;
				if(size &&
				    sse::encode({std::string_view(text, size), "diagnostic", id, 1500}, frame) == sse::EncodeResult::Ok)
				{
					std::array<sse::Token, sse::client_capacity> clients;
					portENTER_CRITICAL(&event_clients_lock);
					clients = event_clients;
					portEXIT_CRITICAL(&event_clients_lock);
					for(auto token : clients)
					{
						const auto result = diagnostic_events.publish(token, frame);
						if(result != sse::Publish::Queued && result != sse::Publish::Closed)
							count_rejected();
					}
				}
			}

			// Shutdown owner must stop this producer, drain diagnostic_events.stop(), then server.end().
			const auto state = runtime_state();
			bool fast = touch_lcd7::snapshot().touch.pressed || controls_changed.exchange(false) ||
			    state.experiment.running || (state.strip.enabled && state.strip.mode != 0);
			if(!fast && tick % 10)
				continue;

			const unsigned clients = websocket.count();
			if(!clients)
				continue;

			Writer writer(producer_buffer, sizeof(producer_buffer));
			encode_summary(writer, clients, true);
			auto size = writer.size(producer_buffer);
			if(size)
			{
				const auto sent = websocket.send_binary(producer_buffer, size);
				count_tx(size, sent.queued);
				if(sent.rejected)
					count_rejected();
			}
			else
				count_rejected();
		}
	}

} // namespace
namespace touch_lcd7
{
	void start_web()
	{
		if(tcp_transport.start().status != tcp::Status::Ok)
			std::abort();

		ESP_ERROR_CHECK(lcd7_web::runtime_init());
		lcd7_web::telemetry_init();
		if(!diagnostic_events.start())
			std::abort();

		server.onNotFound(
			[](Request &, Response &response)
		{
			error(response, 404, "not_found");
		});
		websocket.onMessage(ws_message);
#if NAWSERVER_MCP
		(void)nawserver_demo_mcp::initialize();
#endif
		if(server.setHeaderPipeline(http_counter_chain) != HttpError::Ok) std::abort();
	if(server.begin() != HttpError::Ok)
			std::abort();

		if(xTaskCreate(producer, "web_telemetry", 6144, nullptr, 2, nullptr) != pdPASS)
			std::abort();
	}

} // namespace touch_lcd7
