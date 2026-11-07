// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "application.hpp"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <atomic>
#include <cstring>
#if __has_include("wifi_secret.hpp")
#include "wifi_secret.hpp"
#else
#define NAWSERVER_DEMO_LCD7_WIFI_SSID ""
#define NAWSERVER_DEMO_LCD7_WIFI_PASSWORD ""
#endif
namespace touch_lcd7
{
	namespace
	{
		esp_timer_handle_t reconnect_timer = nullptr;
		esp_timer_handle_t connection_deadline_timer = nullptr;
		std::atomic<bool> network_connected{false};
		std::atomic<unsigned> connection_failures{0};
		constexpr uint64_t connection_deadline_microseconds = 20000000;

		void restart_without_ip(void *)
		{
			if(network_connected.load()) return;

			ESP_LOGW("lcd7", "No IP for 20 seconds; restarting by owner Wi-Fi policy");
			esp_restart();
		}

		void arm_connection_deadline()
		{
			// Repeated disconnects must not extend the same 20-second window.
			// A successful DHCP result cancels it; later loss starts a new window.
			if(esp_timer_is_active(connection_deadline_timer)) return;

			const auto result = esp_timer_start_once(connection_deadline_timer, connection_deadline_microseconds);
			if(result != ESP_OK && result != ESP_ERR_INVALID_STATE)
				ESP_LOGW("lcd7", "Wi-Fi deadline scheduling failed: %s", esp_err_to_name(result));
		}

		void connect_station(void *)
		{
			if(network_connected.load()) return;

			const auto result = esp_wifi_connect();
			if(result != ESP_OK)
			{
				ESP_LOGW("lcd7", "Wi-Fi connect request: %s; retry in 30 s", esp_err_to_name(result));
				(void)esp_timer_start_once(reconnect_timer, 30000000);
			}
		}

	}

	static void network_event(void *, esp_event_base_t base, int32_t id, void *data)
	{
		if(base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
		{
			arm_connection_deadline();
			connect_station(nullptr);
		}
		else if(base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED)
		{
			const auto *event = static_cast<const wifi_event_sta_connected_t *>(data);
			ESP_LOGI("lcd7", "Wi-Fi associated, channel=%u, auth=%u", event->channel, event->authmode);
		}
		else if(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
		{
			const auto *event = static_cast<const wifi_event_sta_disconnected_t *>(data);
			network_connected.store(false);
			arm_connection_deadline();
			const unsigned failures = connection_failures.load();
			const unsigned delay_ms = 1000U << (failures < 5 ? failures : 5);
			const unsigned bounded_delay_ms = delay_ms < 30000 ? delay_ms : 30000;
			if(failures < 5) connection_failures.store(failures + 1);

			// A transient missing hidden AP must not leave this unattended demo
			// offline permanently. Retry with capped backoff, without blocking the
			// event task or adding a polling task; never erase NVS or change auth.
			ESP_LOGW("lcd7", "Wi-Fi disconnected, reason=%u; retry in %u ms", event->reason, bounded_delay_ms);
			(void)esp_timer_stop(reconnect_timer);
			const auto scheduled = esp_timer_start_once(reconnect_timer,
			        static_cast<uint64_t>(bounded_delay_ms) * 1000);
			if(scheduled != ESP_OK && scheduled != ESP_ERR_INVALID_STATE)
				ESP_LOGW("lcd7", "Wi-Fi retry scheduling failed: %s", esp_err_to_name(scheduled));
		}
		else if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
		{
			network_connected.store(true);
			connection_failures.store(0);
			(void)esp_timer_stop(reconnect_timer);
			(void)esp_timer_stop(connection_deadline_timer);
			auto *event = static_cast<ip_event_got_ip_t *>(data);
			ESP_LOGI("lcd7", "IP: " IPSTR, IP2STR(&event->ip_info.ip));
		}
		else if(base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)
		{
			network_connected.store(false);
			arm_connection_deadline();
			ESP_LOGW("lcd7", "IP lost; 20-second connection deadline started");
		}
	}

	void start_network()
	{
		if(!std::strlen(NAWSERVER_DEMO_LCD7_WIFI_SSID))
		{
			ESP_LOGW("lcd7", "No local Wi-Fi header; LCD/touch stay available offline");
			return;
		}

		// No automatic NVS erase and no credential import from other demos.
		ESP_ERROR_CHECK(nvs_flash_init());
		esp_netif_create_default_wifi_sta();
		wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
		ESP_ERROR_CHECK(esp_wifi_init(&config));
		ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
		// This powered LCD demo keeps HTTP/WS/SSE active, as the C6 demo does.
		ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
		esp_timer_create_args_t timer_arguments{};
		timer_arguments.callback = connect_station;
		timer_arguments.name = "lcd7_wifi_retry";
		ESP_ERROR_CHECK(esp_timer_create(&timer_arguments, &reconnect_timer));
		timer_arguments.callback = restart_without_ip;
		timer_arguments.name = "lcd7_wifi_deadline";
		ESP_ERROR_CHECK(esp_timer_create(&timer_arguments, &connection_deadline_timer));
		ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, nullptr));
		ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, network_event, nullptr));
		wifi_config_t wifi{};
		strlcpy(reinterpret_cast<char *>(wifi.sta.ssid), NAWSERVER_DEMO_LCD7_WIFI_SSID, sizeof(wifi.sta.ssid));
		strlcpy(reinterpret_cast<char *>(wifi.sta.password), NAWSERVER_DEMO_LCD7_WIFI_PASSWORD, sizeof(wifi.sta.password));
		// Search all channels and prefer the strongest matching AP. Fast scan
		// stops at its first match; the hidden mesh SSID can have several BSSIDs.
		// A channel/BSSID is deliberately not pinned to today's observation.
		wifi.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
		wifi.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
		wifi.sta.failure_retry_cnt = 2;
		ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
		ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
		ESP_ERROR_CHECK(esp_wifi_start());
	}

} // namespace touch_lcd7
extern "C" void app_main()
{
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	if(!touch_lcd7::start_application())
	{
		ESP_LOGE("lcd7", "Application task allocation failed");
		return;
	}

	touch_lcd7::start_web();
	touch_lcd7::start_network();
}
