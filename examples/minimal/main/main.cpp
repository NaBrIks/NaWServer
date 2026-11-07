// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <NaWServer/http_server.h>
#include <NaWServer/handler_middleware.h>
#include <NaWServer/route.h>
#include <NaWServer/handler_static.h>
#include <NaWServer/websocket.h>
#include <NaWServer/sse.h>
#include <NaWServer/backends/tcp_transmission.h>
#include <NaWServer/backends/esp_idf_tcp.h>
#include <NaWServer/backends/tcp.h>
#include <lwip/opt.h>
#include <NaWServer/backends/tcp_sse.h>
#if !CONFIG_NAWSERVER_WEBSOCKET || !CONFIG_NAWSERVER_SSE
#error "The minimal diagnostic sample requires NaWServer WebSocket and SSE support"
#endif
#include "../../common/sse_test.hpp"
#include "http_test_assets.hpp"
#include "../../common/demo_mcp.hpp"
#if __has_include("wifi_secret.hpp")
#include "wifi_secret.hpp"
#else
#define NAWSERVER_DEMO_WIFI_SSID ""
#define NAWSERVER_DEMO_WIFI_PASSWORD ""
#endif
using namespace NaWServer;
static HandlerStaticUriText index_page("/", http_test_assets::html, sizeof(http_test_assets::html) - 1, "text/html");
static HandlerStaticUriText test_stylesheet("/http-test.css", http_test_assets::css, sizeof(http_test_assets::css) - 1,
    "text/css; charset=utf-8");
static HandlerStaticUriText test_script("/http-test.js", http_test_assets::javascript,
    sizeof(http_test_assets::javascript) - 1, "text/javascript; charset=utf-8");
static HandlerStaticUriText test_configuration("/http-test-config.json", http_test_assets::configuration,
    sizeof(http_test_assets::configuration) - 1, "application/json");
static HandlerStaticUriText mcp_client_script("/mcp-client.js", http_test_assets::mcp_client,
    sizeof(http_test_assets::mcp_client) - 1, "text/javascript; charset=utf-8");
static HandlerStaticUriText mcp_panel_script("/mcp-panel.js", http_test_assets::mcp_panel,
    sizeof(http_test_assets::mcp_panel) - 1, "text/javascript; charset=utf-8");
static WebSocketEndpoint websocket("/ws");
static FixedBasicAuthenticator test_users("test", "test-only");
static AuthMiddleware auth(test_users);
static AccessMiddleware access_check;
static auto private_chain = makeHeaderChain(auth, access_check);
static tcp::IdfTransport tcp_transport;
static TcpOptions tcp_options()
{
	TcpOptions options;
	options.bind_address = "0.0.0.0";
	options.port = 80;
	options.transmission.enabled = true;
	options.transmission.block_bytes = 4 * TCP_MSS;
	options.transmission.step_bytes = TCP_MSS;
	return options;
}

static TcpHttpBackend http_backend(tcp_transport, tcp_options());
static sse::TcpAdapter sse_backend(http_backend);
static sse::EventSource events(sse_backend);
static examples::SseTest sse_test(events, private_chain);
inline constexpr Header commonHeaders[]{{"X-Response-API", "sync-v1"}, {"X-Common", "server"}};
inline constexpr DefaultHeaders defaultHeaders{commonHeaders};
inline constexpr ResponseOptions apiResponse{ContentType::Json, CachePolicy::NoStore, HeaderFlags::NoSniff};
static SendResult health(Response &response)
{
	if(response.header("X-Url-Decoder", urlDecodeBackend()) != SendResult::Ok)
		return response.error();

	return response.text(200, "{\"ok\":true}");
}

static SendResult echo(Request &request, Response &response)
{
	const auto value = request.parameter("value").value_or(std::string_view{});
	return response.send(
		200,
		"application/octet-stream",
		{reinterpret_cast<const uint8_t *>(value.data()), value.size()});
}

static Route health_route("/health", HttpMethod::Get, health, {}, AccessPolicy::public_access(), apiResponse);
static Route echo_route("/echo", Route::any_method, echo);
static SendResult private_page(Response &response)
{
	return response.text(200, "authorized");
}

static Route private_route("/private", Route::any_method, private_page, {}, AccessPolicy::authenticated(), {}, private_chain);
static SendResult body_echo(Request &request, Response &response)
{
	const auto bytes = request.body();
	const auto mode = request.header("X-Body-Copy").value_or("raw");
	if(mode == "raw")
		return response.bytes(200, bytes);

	if(mode == "vector")
	{
		std::vector<uint8_t> owned;
		if(request.copyBodyTo(owned) != BodyCopyResult::Ok)
			return SendResult::InvalidArgument;

		return response.bytes(200, owned.data(), owned.size());
	}

	if(mode == "array")
	{
		std::array<uint8_t, 64> owned{};
		if(request.copyBodyTo(owned) != BodyCopyResult::Ok)
			return response.text(413, "Copy buffer too small");

		return response.bytes(200, owned.data(), bytes.size());
	}

	if(mode == "pointer" || mode == "c-array")
	{
		uint8_t owned[64]{};
		const auto result = mode == "pointer" ? request.copyBodyTo(owned, sizeof(owned)) : request.copyBodyTo(owned);
		if(result != BodyCopyResult::Ok)
			return response.text(413, "Copy buffer too small");

		return response.bytes(200, owned, bytes.size());
	}

	return response.text(400, "Unknown copy mode");
}

static Route body_route("/private/body", HttpMethod::Post, body_echo, {}, AccessPolicy::authenticated(), {}, private_chain);
static SendResult response_examples(Request &request, Response &response)
{
	const auto path = request.path();
	if(path == "/response/metrics")
	{
		char report[512];
		const auto transmission = http_backend.transmissionStats();
		const int count = std::snprintf(
			report,
			sizeof(report),
			"{\"heap_free\":%u,\"heap_min\":%u,\"largest_block\":%u,\"http_stack_min_free\":%u,"
			"\"ws_clients\":%u,\"tx_jobs\":%u,\"tx_bytes\":%u,\"tx_failed\":%u}",
			static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
			static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
			static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
			static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
			static_cast<unsigned>(websocket.count()),
			static_cast<unsigned>(transmission.active_jobs),
			static_cast<unsigned>(transmission.used_bytes),
			static_cast<unsigned>(transmission.failed_jobs));
		if(count < 0 || static_cast<std::size_t>(count) >= sizeof(report))
			return SendResult::BufferTooSmall;

		return response.send(
			200,
			"application/json",
			{reinterpret_cast<const uint8_t *>(report), static_cast<std::size_t>(count)});
	}

	if(path == "/response/empty")
		return response.empty();

	if(path == "/response/redirect")
		return response.redirect("/health");

	if(path == "/response/buffer")
	{
		TextBuffer<128> text;
		text.print("value=");
		text.print(request.parameter("value").value_or("none"));
		text.printf("; n=%d", 42);
		if(!text.valid())
			return response.text(413, "Text buffer full");

		return response.text(200, text);
	}

	if(path == "/response/headers")
	{
		if(response.options(apiResponse) != SendResult::Ok)
			return response.error();

		if(response.header("x-common", "endpoint") != SendResult::Ok)
			return response.error();

		if(response.header("Cache-Control", "private") != SendResult::Ok)
			return response.error();

		return response.text(200, "{\"headers\":true}");
	}

	if(path == "/response/read-header")
	{
		char value[32];
		const auto read = request.readHeader("X-Test", value, sizeof(value));
		if(read.status != HeaderReadStatus::Ok)
			return response.text(400, "Header missing or too long");

		return response.text(200, {value, read.size});
	}

	if(path == "/response/duplicate")
	{
		(void)response.text(200, "first");
		return response.text(200, "second");
	}

	return response.text(404, "Unknown response example");
}

class ResponseExamples : public HandlerBase
{
	void handleRequest(Request *request, Response &response) override
	{
		(void)response_examples(*request, response);
	}

	RouteDescriptors routes() const override
    {
        static constexpr RouteDescriptor descriptors[] = {{"/response/", HttpMethod::Any, RouteMatch::Prefix}};
        return {descriptors, 1};
    }

} response_examples_handler;
// Response-only endpoints still go through exactly the same access pipeline.
static Route
    protected_response_only("/response/private", HttpMethod::Get, private_page, {}, AccessPolicy::authenticated(), {}, private_chain);
static constexpr uint8_t gzip_fixture[]{0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xcb, 0x48, 0xcd,
	                                    0xc9, 0xc9, 0x07, 0x00, 0x86, 0xa6, 0x10, 0x36, 0x05, 0x00, 0x00, 0x00};
static HandlerStaticUriBin gzip_route("/response/static-gzip", gzip_fixture, sizeof(gzip_fixture), "text/html", true);
static HandlerBase *const route_table[]{
#if NAWSERVER_MCP
	&nawserver_demo_mcp::read_only_endpoint<>,
#endif
	&mcp_client_script,
	&mcp_panel_script,
	&index_page,
	&test_stylesheet,
	&test_script,
	&test_configuration,
	&health_route,
	&echo_route,
	&private_route,
	&body_route,
	&protected_response_only,
	&gzip_route,
	&response_examples_handler,
	&websocket,
	&sse_test.open_route, &sse_test.private_route, &sse_test.publish_route,
	&sse_test.close_route, &sse_test.stats_route, &sse_test.stop_route};
static HttpServer server(http_backend, route_table, {}, defaultHeaders);
static std::atomic<bool> http_start_requested{false};
void run_loopback_tests(NaWServer::HttpServer &server);
void run_response_checks();
static void check_http(HttpError result)
{
	if(result == HttpError::Ok) return;

	ESP_LOGE("sample", "HTTP lifecycle failed, error=%u", static_cast<unsigned>(result));
	std::abort();
}

static void on_wifi(void *, esp_event_base_t base, int32_t id, void *data)
{
	if(base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
		esp_wifi_connect();
	else if(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
	{
		static unsigned retries = 0;
		auto *event = static_cast<wifi_event_sta_disconnected_t *>(data);
		ESP_LOGW("sample", "Wi-Fi disconnected, reason=%u", event->reason);
		if(retries++ < 10)
			esp_wifi_connect();
		else
			ESP_LOGW("sample", "Reconnect limit reached; network admission may be required");
	}
	else if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
	{
		auto *event = static_cast<ip_event_got_ip_t *>(data);
		ESP_LOGI("sample", "IP: " IPSTR, IP2STR(&event->ip_info.ip));
		http_start_requested.store(true);
	}
}

extern "C" void app_main()
{
	ESP_ERROR_CHECK(nvs_flash_init()); // Never silently erase the owner's NVS.
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	const auto transport_started = tcp_transport.start();
	if(transport_started.status != tcp::Status::Ok)
	{
		ESP_LOGE("sample", "TCP transport start failed, status=%u native=%d",
		    static_cast<unsigned>(transport_started.status), transport_started.native_error);
		return;
	}


	websocket.onMessage(
		[](WebSocketClient *client, const uint8_t *data, size_t len, bool binary)
	{
		WebSocketSendResult result;
		if(binary)
			result = client->send_binary(data, len);
		else
		{
			std::string text(reinterpret_cast<const char *>(data), len);
			result = client->send(text.c_str());
		}

		if(result != WebSocketSendResult::Queued)
			ESP_LOGW("sample", "WS echo not queued, reason=%u", static_cast<unsigned>(result));
	});
#if NAWSERVER_MCP
	if(!nawserver_demo_mcp::initialize()) { ESP_LOGE("sample", "MCP workspace unavailable"); return; }

#endif
	check_http(server.begin());
	vTaskDelay(pdMS_TO_TICKS(3000)); // Let the USB test console attach after flash.
	run_response_checks();
	run_loopback_tests(server);
	ESP_LOGI("sample", "main stack minimum free=%u bytes", unsigned(uxTaskGetStackHighWaterMark(nullptr)));
	if(!events.start()) { ESP_LOGE("sample", "SSE start failed"); return; }

	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, nullptr));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, nullptr));
	if(std::strlen(NAWSERVER_DEMO_WIFI_SSID) == 0)
	{
		ESP_LOGW("sample", "Set ignored main/wifi_secret.hpp before using Wi-Fi");
		return;
	}

	esp_netif_create_default_wifi_sta();
	wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&config));
	// Keep server latency predictable during transport acceptance tests.
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	wifi_config_t wifi{};
	strlcpy(reinterpret_cast<char *>(wifi.sta.ssid), NAWSERVER_DEMO_WIFI_SSID, sizeof(wifi.sta.ssid));
	strlcpy(reinterpret_cast<char *>(wifi.sta.password), NAWSERVER_DEMO_WIFI_PASSWORD, sizeof(wifi.sta.password));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
	ESP_ERROR_CHECK(esp_wifi_start());
	TickType_t last_websocket_tick = xTaskGetTickCount();
	for(;;)
	{
		vTaskDelay(pdMS_TO_TICKS(20));
		if(http_start_requested.exchange(false))
		{
			check_http(server.begin());
			ESP_LOGI("sample", "HTTP server ready");
		}

		sse_test.poll();
		if(sse_test.stop_requested.exchange(false))
		{
			const bool drained = events.stop(5000);
			const auto state = events.stats();
			ESP_LOGI("sample", "SSE_STOP drained=%u active=%u begun=%u completed=%u queued=%u errors=%u",
			    unsigned(drained), unsigned(state.active), unsigned(state.begun), unsigned(state.completed),
			    unsigned(state.queued_bytes), unsigned(events.close_errors()));
			if(drained) { check_http(server.end()); check_http(server.begin()); }
		}

		const TickType_t current_tick = xTaskGetTickCount();
		if(current_tick - last_websocket_tick < pdMS_TO_TICKS(5000))
			continue;

		last_websocket_tick = current_tick;
		const auto result = websocket.send("tick");
		if(result.rejected)
			ESP_LOGW("sample", "WS tick rejected for %u clients", static_cast<unsigned>(result.rejected));
	}
}
