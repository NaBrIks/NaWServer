// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/sse.h>
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/backends/esp_idf_tcp.h>
#include <NaWServer/backends/tcp.h>
#include <NaWServer/request.h>
#include <NaWServer/http_server.h>
#include <NaWServer/handler_middleware.h>
#include <NaWServer/route.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <cstring>
#include <cstdlib>
#if __has_include("wifi_secret.hpp")
#include "wifi_secret.hpp"
#else
#define NAWSERVER_DEMO_WIFI_SSID ""
#define NAWSERVER_DEMO_WIFI_PASSWORD ""
#endif
using namespace NaWServer;
namespace
{
	tcp::IdfTransport transport;
	TcpOptions tcp_options()
	{
		TcpOptions options;
		options.bind_address = "0.0.0.0";
		options.port = 80;
		return options;
	}
	TcpHttpBackend backend(transport, tcp_options());
	sse::TcpAdapter events_backend(backend);
	sse::EventSource events(events_backend);
	std::array<sse::Token, sse::client_capacity> clients{}; // accessed only by HTTP callbacks
	FixedBasicAuthenticator users("test", "test-only", 1); // public example credentials
	AuthMiddleware auth(users);
	AccessMiddleware access;
	auto guarded = makeHeaderChain(auth, access);
	constexpr Header defaults[] = {{"X-Example", "library-sse"}};
	SendResult subscribe(Request &request, Response &response)
	{
		const auto opened = events.open(request, response);
		if(opened.result == SendResult::Ok)
		{
			clients[opened.token.slot] = opened.token;
			return SendResult::Ok;
		}

		if(response.state() == ResponseState::External)
			return opened.result;

		return response.text(opened.admission == sse::Admit::Full ? 503 : 400, "SSE could not open");
	}

	SendResult publish(Request &request, Response &response)
	{
		const auto bytes = request.body();
		const std::string_view text = bytes.size()
		                                  ? std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size())
		                                  : std::string_view{};
		sse::Frame frame;
		const auto encoded = sse::encode({text, "update", {}, 1000}, frame);
		if(encoded != sse::EncodeResult::Ok)
			return response.text(encoded == sse::EncodeResult::TooLarge ? 413 : 400, "Invalid UTF-8/event size");

		unsigned queued = 0, refused = 0;
		for(auto token : clients)
		{
			const auto result = events.publish(token, frame);
			if(result == sse::Publish::Queued)
				++queued;
			else if(result != sse::Publish::Closed)
				++refused;
		}
		TextBuffer<96> result;
		result.printf("queued=%u refused=%u; queued is not delivery ACK", queued, refused);
		return response.text(200, result);
	}

	SendResult health(Response &response)
	{
		return response.text(200, "library SSE example ready");
	}

	Route subscription("/events", HttpMethod::Get, subscribe, {}, AccessPolicy::require(1), {}, guarded);
	Route publication("/publish", HttpMethod::Post, publish, {}, AccessPolicy::require(1), {}, guarded);
	Route health_route("/health", HttpMethod::Get, health);
	HandlerBase *const routes[] = {&subscription, &publication, &health_route};
	HttpServer server(backend, routes, {}, DefaultHeaders(defaults));
	void network(void *, esp_event_base_t base, int32_t id, void *data)
	{
		if(base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
			esp_wifi_connect();

		if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
		{
			auto *got = static_cast<ip_event_got_ip_t *>(data);
			ESP_LOGI("sse_example", "Ready at " IPSTR, IP2STR(&got->ip_info.ip));
		}
	}

} // namespace
extern "C" void app_main()
{
	ESP_ERROR_CHECK(nvs_flash_init()); // Never erase the owner's NVS as a fallback.
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	if(!events.start())
		std::abort(); // explicit runtime startup after SDK prerequisites

	if(transport.start().status != tcp::Status::Ok) std::abort();
	if(server.begin() != HttpError::Ok) std::abort();
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network, nullptr));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network, nullptr));
	if(!std::strlen(NAWSERVER_DEMO_WIFI_SSID))
	{
		ESP_LOGW("sse_example", "Provide ignored wifi_secret.hpp");
		return;
	}

	esp_netif_create_default_wifi_sta();
	wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&init));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	wifi_config_t wifi{};
	strlcpy(reinterpret_cast<char *>(wifi.sta.ssid), NAWSERVER_DEMO_WIFI_SSID, sizeof(wifi.sta.ssid));
	strlcpy(reinterpret_cast<char *>(wifi.sta.password), NAWSERVER_DEMO_WIFI_PASSWORD, sizeof(wifi.sta.password));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	ESP_ERROR_CHECK(esp_wifi_start());
	// An application that shuts down must stop publishers, wait events.stop(),
	// then call server.end(); never stop from a URI callback.
}
