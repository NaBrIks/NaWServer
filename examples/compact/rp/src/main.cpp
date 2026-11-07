// SPDX-License-Identifier: MIT
#include "../../common/demo.hpp"
#include "../../platform/rp.hpp"
#include <NaWServer/backends/lwip_tcp.h>
#include <FreeRTOS.h>
#include <task.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/bootrom.h"
#include "lwip/netif.h"
#include "wifi_config.h"
#include <cstdio>
#include <atomic>
#include <cstring>
#include <malloc.h>
#if NAWSERVER_DEMO_RP_STARTUP_MEASURE
#include "hardware/watchdog.h"
#include "lwip/sockets.h"
#endif

#ifndef NAWSERVER_DEMO_RP_STARTUP_MEASURE
#define NAWSERVER_DEMO_RP_STARTUP_MEASURE 0
#endif
#ifndef NAWSERVER_DEMO_RP_NO_WEB_BASELINE
#define NAWSERVER_DEMO_RP_NO_WEB_BASELINE 0
#endif

namespace
{
	compact::RpPlatform platform;
	std::atomic<bool> ready{false};
	std::atomic<bool> status_requested{false};
	std::atomic<bool> scan_requested{false};
	std::atomic<unsigned> scan_count{0}, scan_matches{0}, scan_channel{0}, scan_auth{0};
	std::atomic<int> scan_rssi{-127};
#if NAWSERVER_DEMO_RP_STARTUP_MEASURE
	struct StartupMemory { size_t rtos_free, rtos_largest, cpp_used; unsigned tasks; };
	StartupMemory startup_memory[7]{};
	bool startup_wifi_ready = false;
	bool startup_boot_info_valid = false, startup_tbyb_pending = true, startup_watchdog_reset = false;
	std::atomic<bool> stop_probe_requested{false};
	std::atomic<bool> reboot_probe_requested{false};
	int startup_probe_listener = -1;
	unsigned startup_probe_cycles = 0;
	absolute_time_t startup_probe_deadline;
	StartupMemory currentMemory()
	{
		HeapStats_t heap{}; vPortGetHeapStats(&heap);
		const auto native = mallinfo();
		return {heap.xAvailableHeapSpaceInBytes, heap.xSizeOfLargestFreeBlockInBytes,
		    static_cast<size_t>(native.uordblks), unsigned(uxTaskGetNumberOfTasks())};
	}
	void startupSample(unsigned phase)
	{
		startup_memory[phase] = currentMemory();
	}
	void stopSample(const char *stage)
	{
		HeapStats_t heap{}; vPortGetHeapStats(&heap);
		const auto native = mallinfo();
		std::printf("COMPACT_STOP_PROBE stage=%s rtos_free=%u rtos_largest=%u cpp_used=%u tasks=%u\n",
		    stage, unsigned(heap.xAvailableHeapSpaceInBytes), unsigned(heap.xSizeOfLargestFreeBlockInBytes),
		    unsigned(native.uordblks), unsigned(uxTaskGetNumberOfTasks()));
	}
	void openStartupProbe()
	{
		if(!startup_wifi_ready || startup_tbyb_pending) return;
		const int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
		if(fd < 0) { std::puts("COMPACT_STARTUP_ENDPOINT socket_failed"); return; }
		sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = PP_HTONS(81);
		address.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
		const int one = 1;
		(void)lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if(lwip_bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
		    lwip_listen(fd, 1) != 0 || lwip_fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
		{
			lwip_close(fd); std::puts("COMPACT_STARTUP_ENDPOINT bind_failed"); return;
		}
		startup_probe_listener = fd;
		startup_probe_deadline = make_timeout_time_ms(60000);
		std::puts("COMPACT_STARTUP_ENDPOINT open");
	}
	void pollStartupProbe(compact::Demo &demo, NaWServer::TcpHttpBackend &backend)
	{
		if(startup_probe_listener < 0) return;
		const int client = lwip_accept(startup_probe_listener, nullptr, nullptr);
		if(client < 0)
		{
			if(time_reached(startup_probe_deadline))
			{
				lwip_close(startup_probe_listener); startup_probe_listener = -1;
				std::puts("COMPACT_STARTUP_ENDPOINT timeout");
			}
			return;
		}
		lwip_close(startup_probe_listener); startup_probe_listener = -1;
		(void)lwip_fcntl(client, F_SETFL, O_NONBLOCK);
		// Consume a bounded request header before close to avoid resetting an
		// otherwise valid response due to unread request bytes.
		char request[512]; std::size_t request_used = 0;
		const auto request_deadline = make_timeout_time_ms(500);
		while(request_used + 1 < sizeof(request) && !time_reached(request_deadline))
		{
			const int count = lwip_recv(client, request + request_used, sizeof(request) - 1 - request_used, 0);
			if(count > 0)
			{
				request_used += static_cast<std::size_t>(count); request[request_used] = 0;
				if(std::strstr(request, "\r\n\r\n")) break;
			}
			else vTaskDelay(pdMS_TO_TICKS(5));
		}
		const auto streams_before = demo.activeStreams();
		const auto tx = backend.transmissionStats();
		const bool stop_attempted = streams_before == 0 && !tx.queued_jobs && !tx.active_jobs && !tx.filling_jobs;
		bool stopped = false, restarted = false;
		NaWServer::TcpDiagnostics restart_diagnostics{};
		StartupMemory before_stop{}, after_stop{}, after_restart{};
		if(stop_attempted)
		{
			before_stop = currentMemory();
			stopped = demo.stop();
			if(stopped)
			{
				vTaskDelay(pdMS_TO_TICKS(1000)); after_stop = currentMemory();
				restarted = demo.start();
				restart_diagnostics = backend.diagnostics();
				vTaskDelay(pdMS_TO_TICKS(1000)); after_restart = currentMemory();
			}
		}
		char body[900]; std::size_t used = 0;
		const auto append = [&](const char *format, auto... values)
		{
			if(used >= sizeof(body)) return;
			const int count = std::snprintf(body + used, sizeof(body) - used, format, values...);
			if(count < 0 || static_cast<std::size_t>(count) >= sizeof(body) - used) used = sizeof(body);
			else used += static_cast<std::size_t>(count);
		};
		append("{\"wifi_ready\":true,\"pending\":false,\"boot_info\":%s,\"watchdog_reset\":%s,\"metadata_mode\":%u,\"metadata_arena_bytes\":%u,\"payload_mode\":%u,\"http_jobs\":%u,\"connections\":%u,\"samples\":[",
		    startup_boot_info_valid ? "true" : "false", startup_watchdog_reset ? "true" : "false",
		    unsigned(NAWSERVER_DEMO_TX_METADATA_MODE), unsigned(NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES),
		    unsigned(NAWSERVER_DEMO_TX_MEMORY_MODE), unsigned(NAWSERVER_DEMO_HTTP_JOBS), unsigned(NAWSERVER_DEMO_CONNECTIONS));
		for(unsigned i = 0; i < 7; ++i)
			append("%s[%u,%u,%u,%u]",
			    i ? "," : "", unsigned(startup_memory[i].rtos_free), unsigned(startup_memory[i].rtos_largest),
			    unsigned(startup_memory[i].cpp_used), startup_memory[i].tasks);
		append("],\"stop\":{\"attempted\":%s,\"stopped\":%s,\"restarted\":%s,\"streams_before\":%u,\"queued_before\":%u,\"active_before\":%u,\"filling_before\":%u,\"restart_state\":%d,\"restart_failure\":%d,\"native_error\":%d,\"samples\":[",
		    stop_attempted ? "true" : "false", stopped ? "true" : "false", restarted ? "true" : "false",
		    unsigned(streams_before), unsigned(tx.queued_jobs), unsigned(tx.active_jobs), unsigned(tx.filling_jobs),
		    int(restart_diagnostics.state), int(restart_diagnostics.last_failure), restart_diagnostics.native_error);
		for(unsigned i = 0; i < 3; ++i)
		{
			const auto &value = i == 0 ? before_stop : i == 1 ? after_stop : after_restart;
			append("%s[%u,%u,%u,%u]",
			    i ? "," : "", unsigned(value.rtos_free), unsigned(value.rtos_largest), unsigned(value.cpp_used), value.tasks);
		}
		append("]},\"app_free_stack_words\":%u}", unsigned(uxTaskGetStackHighWaterMark(nullptr)));
		if(used < sizeof(body))
		{
			char response[1152];
			const int header = std::snprintf(response, sizeof(response),
			    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: %u\r\n\r\n", unsigned(used));
			if(header > 0 && static_cast<std::size_t>(header) + used <= sizeof(response))
			{
				std::memcpy(response + header, body, used);
				std::size_t sent = 0, total = static_cast<std::size_t>(header) + used;
				const auto deadline = make_timeout_time_ms(1000);
				while(sent < total && !time_reached(deadline))
				{
					const int count = lwip_send(client, response + sent, total - sent, 0);
					if(count > 0) sent += static_cast<std::size_t>(count);
					else vTaskDelay(pdMS_TO_TICKS(5));
				}
				std::printf("COMPACT_STARTUP_ENDPOINT sent=%u total=%u\n", unsigned(sent), unsigned(total));
			}
		}
		lwip_close(client);
		++startup_probe_cycles;
		if(restarted && startup_probe_cycles < NAWSERVER_DEMO_RP_PROBE_CYCLES) openStartupProbe();
		if(stopped && !restarted)
		{
			watchdog_reboot(0, 0, 100);
			for(;;) vTaskDelay(pdMS_TO_TICKS(1000));
		}
	}
#endif
	int scanned(void *, const cyw43_ev_scan_result_t *result)
	{
		if(!result) return 0;
		++scan_count;
		if(result->ssid_len == std::strlen(NAWSERVER_DEMO_WIFI_SSID) &&
		    !std::memcmp(result->ssid, NAWSERVER_DEMO_WIFI_SSID, result->ssid_len))
		{
			++scan_matches;
			if(result->rssi > scan_rssi) { scan_rssi = result->rssi; scan_channel = result->channel; scan_auth = result->auth_mode; }
		}
		return 0;
	}
	void usb(void *)
	{
		for(;;)
		{
			const auto command = getchar_timeout_us(0);
			if(command == 'b') reset_usb_boot(0, 0);
			if(command == 'r') status_requested = true;
#if NAWSERVER_DEMO_RP_STARTUP_MEASURE && !NAWSERVER_DEMO_RP_NO_WEB_BASELINE
			if(command == 'm') stop_probe_requested = true;
			if(command == 'n') reboot_probe_requested = true;
#endif
			if(command == 's') scan_requested = true;
			if(command == 'k' && ready.load()) (void)platform.recoverKey();
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}
	void application(void *)
	{
		if(cyw43_arch_init() || !platform.initialize())
		{
			std::puts("COMPACT_INIT_FAILED partition/FS/key/wifi; no format performed");
			for(;;) vTaskDelay(pdMS_TO_TICKS(1000));
		}
		ready = true;
		cyw43_arch_enable_sta_mode();
		// USB-powered demo: avoid radio sleep delaying upload receive windows.
		const auto pm_result = cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM & ~0x0fu);
		std::uint32_t actual_pm = 0;
		const auto pm_read = cyw43_wifi_get_pm(&cyw43_state, &actual_pm);
#if NAWSERVER_DEMO_RP_STARTUP_MEASURE
		// Diagnostic build only: associate before constructing any web objects.
		// Bound the wait so a missing access point cannot prevent normal startup.
		// Do not wait for Wi-Fi while the ROM trial watchdog is pending. The
		// normal server must start and be confirmed first. The operator may then
		// request one reboot to measure a confirmed image after association.
		boot_info_t boot{};
		startup_boot_info_valid = rom_get_boot_info(&boot);
		startup_tbyb_pending = !startup_boot_info_valid || (boot.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING);
		startup_watchdog_reset = watchdog_caused_reboot();
		if(!startup_tbyb_pending)
		{
			const auto wifi_result = cyw43_arch_wifi_connect_timeout_ms(NAWSERVER_DEMO_WIFI_SSID,
			    NAWSERVER_DEMO_WIFI_PASSWORD, NAWSERVER_DEMO_WIFI_PREFER_WPA3 ? CYW43_AUTH_WPA3_SAE_AES_PSK : CYW43_AUTH_WPA2_AES_PSK, 20000);
			startup_wifi_ready = wifi_result == 0;
		}
		if(startup_wifi_ready) vTaskDelay(pdMS_TO_TICKS(1000));
		startupSample(0);
		vTaskDelay(pdMS_TO_TICKS(1000));
		startupSample(1);
#endif
#if NAWSERVER_DEMO_RP_NO_WEB_BASELINE
		constexpr bool started = false;
#else
		static NaWServer::tcp::LwipTransport transport;
#if NAWSERVER_DEMO_RP_STARTUP_MEASURE
		startupSample(2);
#endif
		if(transport.start().status != NaWServer::tcp::Status::Ok) vTaskDelete(nullptr);
#if NAWSERVER_DEMO_RP_STARTUP_MEASURE
		startupSample(3);
#endif
		auto options = compact::Demo::options();
		// Routed Wi-Fi and WPA power saving can delay individual segments.
		options.managed.header_timeout_ms = 5000;
		options.no_progress_timeout_ms = 30000;
		static NaWServer::TcpHttpBackend backend(transport, options);
		static compact::Demo demo(backend, platform);
#if NAWSERVER_DEMO_RP_STARTUP_MEASURE
		startupSample(4);
#endif
		// Bind 0.0.0.0 before association/DHCP. Local firmware health must not
		// depend on how quickly an external access point grants connectivity.
		const bool started = demo.start();
#if NAWSERVER_DEMO_RP_STARTUP_MEASURE
		startupSample(5);
		if(started && startup_wifi_ready) platform.markHealthy(true);
		vTaskDelay(pdMS_TO_TICKS(1000)); startupSample(6);
		openStartupProbe();
#endif
#endif
		if(!started) std::puts("COMPACT_HTTP_START_FAILED; TBYB remains pending");
		bool connecting = false, reported = false;
		bool scanning = false;
		bool wpa3 = NAWSERVER_DEMO_WIFI_PREFER_WPA3 != 0;
		int previous_status = 999;
		auto retry = get_absolute_time(), deadline = nil_time;
#if NAWSERVER_DEMO_RP_STARTUP_MEASURE && !NAWSERVER_DEMO_RP_NO_WEB_BASELINE
		auto automatic_reboot_at = nil_time;
#endif
		for(;;)
		{
#if NAWSERVER_DEMO_RP_STARTUP_MEASURE && !NAWSERVER_DEMO_RP_NO_WEB_BASELINE
			pollStartupProbe(demo, backend);
#endif
			const auto status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
			if(scan_requested.exchange(false) && status != CYW43_LINK_UP && !scanning)
			{
				cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA); connecting = false;
				scan_count = scan_matches = 0; scan_rssi = -127; scan_channel = scan_auth = 0;
				cyw43_wifi_scan_options_t scan{};
				const auto result = cyw43_wifi_scan(&cyw43_state, &scan, nullptr, scanned);
				scanning = result == 0;
				std::printf("COMPACT_SCAN_STARTED result=%d\n", result);
			}
			if(scanning)
			{
				if(!cyw43_wifi_scan_active(&cyw43_state))
				{
					std::printf("COMPACT_SCAN_DONE total=%u configured_matches=%u strongest_channel=%u rssi=%d auth=%u\n",
					    scan_count.load(), scan_matches.load(), scan_channel.load(), scan_rssi.load(), scan_auth.load());
					scanning = false; retry = make_timeout_time_ms(3000);
				}
				platform.poll(); vTaskDelay(pdMS_TO_TICKS(10)); continue;
			}
			const bool requested = status_requested.exchange(false);
			if(status != previous_status || requested)
			{
				char address[16]; cyw43_arch_lwip_begin();
				ip4addr_ntoa_r(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]), address, sizeof(address));
				cyw43_arch_lwip_end();
				std::printf("COMPACT_STATUS link=%d ip=%s web=%u heap=%u min_heap=%u mode=%s slot=%s\n",
				    status, address, started, unsigned(xPortGetFreeHeapSize()), unsigned(xPortGetMinimumEverFreeHeapSize()),
				    wpa3 ? "WPA3" : "WPA2", compact::rp::Ota::runningSlot());
				previous_status = status;
				if(requested)
				{
#if NAWSERVER_DEMO_RP_STARTUP_MEASURE
					std::printf("COMPACT_STARTUP wifi_ready=%u boot_info=%u pending=%u watchdog_reset=%u\n",
					    startup_wifi_ready, startup_boot_info_valid, startup_tbyb_pending, startup_watchdog_reset);
					for(unsigned index = 0; index < 7; ++index)
						std::printf("COMPACT_STARTUP phase=%u rtos_free=%u rtos_largest=%u cpp_used=%u tasks=%u\n",
						    index, unsigned(startup_memory[index].rtos_free), unsigned(startup_memory[index].rtos_largest),
						    unsigned(startup_memory[index].cpp_used), startup_memory[index].tasks);
#endif
#if NAWSERVER_DEMO_RP_TASK_DIAGNOSTICS
					// A bounded snapshot on explicit USB request only. The FreeRTOS
					// watermark is reported in stack words, not bytes.
					static TaskStatus_t tasks[24];
					const auto task_count = uxTaskGetSystemState(tasks, 24, nullptr);
					std::printf("COMPACT_TASKS count=%u total=%u word_bytes=%u\n", unsigned(task_count),
					    unsigned(uxTaskGetNumberOfTasks()), unsigned(sizeof(StackType_t)));
					for(UBaseType_t index = 0; index < task_count; ++index)
						std::printf("COMPACT_TASK name=%s free_min_words=%u\n", tasks[index].pcTaskName,
						    unsigned(tasks[index].usStackHighWaterMark));
#endif
					const auto metrics = platform.status();
					std::printf("COMPACT_METRICS cpu_x100=%u cpu_valid=%u cores=%lu rtos_total=%u rtos_free=%u rtos_largest=%u cpp_total=%u cpp_free=%u static_main=%u physical=%u\n",
					    unsigned(metrics.cpu_percent * 100), metrics.cpu_available,
					    static_cast<unsigned long>(metrics.cpu_cores), unsigned(metrics.total_heap), unsigned(metrics.free_heap),
					    unsigned(metrics.largest_free_block), unsigned(metrics.secondary_heap_total), unsigned(metrics.secondary_heap_free),
					    unsigned(metrics.static_ram_bytes), unsigned(metrics.physical_ram_bytes));
					std::printf("COMPACT_WIFI_PM set=%d read=%d value=%lx\n", pm_result, pm_read, static_cast<unsigned long>(actual_pm));
					const auto settings = platform.settings();
					std::printf("COMPACT_STATE key_revision=%lu name=%s interval_ms=%lu led=%u\n",
					    static_cast<unsigned long>(platform.keyRevision()), settings.name,
					    static_cast<unsigned long>(settings.interval_ms), settings.led);
					const auto timing = compact::rp::flashTiming();
					std::printf("COMPACT_FLASH_TIMING calls=%lu total_us=%llu erase_max_us=%lu program_max_us=%lu read_max_us=%lu\n",
					    static_cast<unsigned long>(timing.calls), static_cast<unsigned long long>(timing.total_us),
					    static_cast<unsigned long>(timing.max_erase_us), static_cast<unsigned long>(timing.max_program_us), static_cast<unsigned long>(timing.max_read_us));
				}
			}
			if(status == CYW43_LINK_UP && !reported)
			{
				connecting = false;
				reported = true;
				char address[16]; cyw43_arch_lwip_begin();
				ip4addr_ntoa_r(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]), address, sizeof(address));
				cyw43_arch_lwip_end(); std::printf("COMPACT_WEB=%s port=80 ready=%u slot=%s\n", address, started, compact::rp::Ota::runningSlot());
			}
			if(status != CYW43_LINK_UP)
			{
				reported = false;
				if(connecting && (status < 0 || time_reached(deadline)))
				{
					cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA); connecting = false;
					wpa3 = !wpa3; retry = make_timeout_time_ms(3000);
				}
				if(!connecting && time_reached(retry))
				{
					connecting = cyw43_arch_wifi_connect_async(NAWSERVER_DEMO_WIFI_SSID, NAWSERVER_DEMO_WIFI_PASSWORD,
					    wpa3 ? CYW43_AUTH_WPA3_SAE_AES_PSK : CYW43_AUTH_WPA2_AES_PSK) == 0;
					// Confirm only after the native network stack, listener and
					// asynchronous station driver all accepted startup.
					if(connecting && started) platform.markHealthy(true);
					if(!connecting) wpa3 = !wpa3;
					deadline = make_timeout_time_ms(30000); retry = make_timeout_time_ms(3000);
				}
			}
#if !NAWSERVER_DEMO_RP_NO_WEB_BASELINE
			#if NAWSERVER_DEMO_RP_STARTUP_MEASURE
			if(started && startup_tbyb_pending && platform.bootConfirmed() && status == CYW43_LINK_UP)
			{
				if(is_nil_time(automatic_reboot_at)) automatic_reboot_at = make_timeout_time_ms(3000);
				if(time_reached(automatic_reboot_at))
				{
					std::puts("COMPACT_REBOOT_PROBE stage=confirmed_automatic");
					vTaskDelay(pdMS_TO_TICKS(50));
					watchdog_reboot(0, 0, 100);
					for(;;) vTaskDelay(pdMS_TO_TICKS(1000));
				}
			}
			if(started && reboot_probe_requested.exchange(false))
			{
				const auto tx = backend.transmissionStats();
				if(status != CYW43_LINK_UP || tx.queued_jobs || tx.active_jobs || tx.filling_jobs)
					std::puts("COMPACT_REBOOT_PROBE stage=busy");
				else
				{
					std::puts("COMPACT_REBOOT_PROBE stage=rebooting");
					vTaskDelay(pdMS_TO_TICKS(50));
					watchdog_reboot(0, 0, 100);
					for(;;) vTaskDelay(pdMS_TO_TICKS(1000));
				}
			}
			if(started && stop_probe_requested.exchange(false))
			{
				const auto tx = backend.transmissionStats();
				if(tx.queued_jobs || tx.active_jobs || tx.filling_jobs)
					std::puts("COMPACT_STOP_PROBE stage=busy");
				else
				{
					stopSample("before_stop");
					const bool stopped = demo.stop();
					std::printf("COMPACT_STOP_PROBE stopped=%u\n", stopped);
					if(stopped)
					{
						vTaskDelay(pdMS_TO_TICKS(1000)); stopSample("after_stop");
						const bool restarted = demo.start();
						std::printf("COMPACT_STOP_PROBE restarted=%u\n", restarted);
						vTaskDelay(pdMS_TO_TICKS(1000)); stopSample("after_restart");
						if(!restarted) watchdog_reboot(0, 0, 100);
					}
				}
			}
			#endif
			if(started) demo.tick();
#endif
			platform.poll(); vTaskDelay(pdMS_TO_TICKS(10));
		}
	}
}
int main()
{
	// Do not disable the ROM TBYB watchdog. A bad candidate must roll back.
	stdio_init_all(); setvbuf(stdout, nullptr, _IONBF, 0);
	std::puts("COMPACT_RP USB b=BOOTSEL k=rotate key r=status s=scan when offline");
	if(xTaskCreate(usb, "usb", 768, nullptr, 4, nullptr) != pdPASS ||
	    xTaskCreate(application, "compact", 2048, nullptr, 4, nullptr) != pdPASS) reset_usb_boot(0, 0);
	vTaskStartScheduler(); reset_usb_boot(0, 0);
}
