#include <FreeRTOS.h>
#include <task.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "lwip/netif.h"
#include "storage.hpp"
#include "api_keys.hpp"
#include "web_demo.hpp"
#include <cstdio>
#include <cstring>
#include <atomic>

namespace
{
	constexpr std::uint32_t startup_guard = 0x5250444d;
	std::atomic<unsigned> requested_commands{0};

	void bootloader()
	{
		watchdog_disable();
		watchdog_hw->scratch[0] = 0;
		reset_usb_boot(0, 0);
	}

	void usbManagement(void *)
	{
		for(;;)
		{
			const auto command = getchar_timeout_us(0);
			#if NAWSERVER_DEMO_API_KEYS
            if(rp_demo::key_console.input(command)) continue;
#endif
			if(command == 'b') bootloader();
			if(command == 'f') requested_commands.fetch_or(1);
			if(command == 'r')
			{
				printf("USB_ALIVE tick=%lu tasks=%u heap=%u\n", static_cast<unsigned long>(xTaskGetTickCount()),
					unsigned(uxTaskGetNumberOfTasks()), unsigned(xPortGetFreeHeapSize()));
				requested_commands.fetch_or(2);
			}
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}

	void application(void *)
	{
		printf("RP_DEMO FreeRTOS %s / Pico SDK %s\n", tskKERNEL_VERSION_NUMBER, PICO_SDK_VERSION_STRING);
		(void)rp_demo::storage().initialize();
		auto settings = rp_demo::storage().settings();
		++settings.boot_count;
		const bool persisted = rp_demo::storage().saveSettings(settings);
		printf("STORAGE fs=%u needs_init=%u nvs_generation=%llu boot=%lu saved=%u\n",
			rp_demo::storage().filesystemMounted(), rp_demo::storage().filesystemNeedsInitialization(),
			static_cast<unsigned long long>(rp_demo::storage().settingsGeneration()),
			static_cast<unsigned long>(settings.boot_count), persisted);
		if(rp_demo::storage().filesystemMounted()) (void)rp_demo::storage().seedWebAssets();
		watchdog_hw->scratch[0] = startup_guard;
		watchdog_enable(8000, false);
		printf("WIFI initialize\n");
		const auto initialized = cyw43_arch_init();
		printf("WIFI initialized result=%d\n", initialized);
		if(initialized) bootloader();
		printf("WIFI enable station\n");
		cyw43_arch_enable_sta_mode();
		watchdog_hw->scratch[0] = 0;
		const auto *mac = cyw43_state.mac;
		printf("WIFI_MAC=%02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		bool server_started = false;
		bool server_start_attempted = false;
		bool connecting = false;
		bool wpa3 = false;
		int previous_status = 999;
		auto retry_at = get_absolute_time();
		auto deadline = nil_time;
		printf("USB b=BOOTSEL f=initialize demo FS r=status\n");
		for(;;)
		{
			watchdog_update();
			const auto commands = requested_commands.exchange(0);
			if(commands & 1)
			{
				const auto mounted = rp_demo::storage().initializeFilesystemExplicitly();
				const auto seeded = mounted && rp_demo::storage().seedWebAssets();
				printf("FS_INITIALIZED mounted=%u seeded=%u\n", mounted, seeded);
			}
			const auto status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
			if(status != previous_status || (commands & 2))
			{
				char address[16];
				cyw43_arch_lwip_begin();
				ip4addr_ntoa_r(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]), address, sizeof(address));
				cyw43_arch_lwip_end();
				printf("WIFI state=%d IP=%s web_ready=%u heap=%u\n",
					status, address, server_started, unsigned(xPortGetFreeHeapSize()));
				previous_status = status;
			}
			if(status == CYW43_LINK_UP)
			{
				connecting = false;
				if(!server_start_attempted)
				{
					server_start_attempted = true;
					server_started = rp_demo::startWebServer();
					printf("WEB_READY=%u port=%u\n", server_started, unsigned(rp_demo::http_port));
				}
				if(server_started) rp_demo::pollWebDemo();
			}
			else
			{
				if(connecting && (status < 0 || time_reached(deadline)))
				{
					cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
					connecting = false;
					wpa3 = !wpa3;
					retry_at = make_timeout_time_ms(3000);
				}
				if(!connecting && time_reached(retry_at))
				{
					printf("WIFI connect SSID=%s mode=%s\n", settings.wifi_ssid, wpa3 ? "WPA3" : "WPA2");
					connecting = cyw43_arch_wifi_connect_async(settings.wifi_ssid, settings.wifi_password,
						wpa3 ? CYW43_AUTH_WPA3_SAE_AES_PSK : CYW43_AUTH_WPA2_AES_PSK) == 0;
					deadline = make_timeout_time_ms(30000);
					retry_at = make_timeout_time_ms(5000);
				}
			}
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}
}

int main()
{
	watchdog_disable();
	if(watchdog_hw->scratch[0] == startup_guard) bootloader();
	stdio_init_all();
	setvbuf(stdout, nullptr, _IONBF, 0);
	// A failed RTOS/storage startup returns to ROM on the next watchdog reset,
	// before touching flash again. USB/network programming can recover it.
	watchdog_hw->scratch[0] = startup_guard;
	if(xTaskCreate(usbManagement, "usb-control", 768, nullptr, 6, nullptr) != pdPASS) bootloader();
	if(xTaskCreate(application, "application", 2048, nullptr, 4, nullptr) != pdPASS) bootloader();
	vTaskStartScheduler();
	bootloader();
}
