// SPDX-License-Identifier: MIT
// RAM-only radio isolation: no filesystem, RTOS, TCP or flash writes.
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/bootrom.h"
#include "wifi_config.h"
#include <cstdio>
#include <cstring>

namespace
{
	unsigned total = 0, matches = 0, channel = 0;
	int best = -127;
	int result(void *, const cyw43_ev_scan_result_t *entry)
	{
		if(!entry) return 0;
		++total;
		if(entry->ssid_len == std::strlen(NAWSERVER_DEMO_WIFI_SSID) &&
		    !std::memcmp(entry->ssid, NAWSERVER_DEMO_WIFI_SSID, entry->ssid_len))
		{
			++matches;
			if(entry->rssi > best) { best = entry->rssi; channel = entry->channel; }
		}
		return 0;
	}
}
int main()
{
	stdio_init_all();
	// Wait for the capture terminal, so scan evidence cannot be lost at boot.
	while(getchar_timeout_us(0) != 's') sleep_ms(10);
	const int initialized = cyw43_arch_init();
	std::printf("RAM_RADIO_INIT=%d\n", initialized);
	if(!initialized)
	{
		cyw43_arch_enable_sta_mode();
		for(unsigned attempt = 1; attempt <= 3; ++attempt)
		{
			total = matches = channel = 0; best = -127;
			cyw43_wifi_scan_options_t options{};
			// The middle pass probes the configured SSID, including hidden APs.
			if(attempt == 2)
			{
				options.ssid_len = std::strlen(NAWSERVER_DEMO_WIFI_SSID);
				std::memcpy(options.ssid, NAWSERVER_DEMO_WIFI_SSID, options.ssid_len);
			}
			const auto started = cyw43_wifi_scan(&cyw43_state, &options, nullptr, result);
			const auto deadline = make_timeout_time_ms(12000);
			while(!started && cyw43_wifi_scan_active(&cyw43_state) && !time_reached(deadline)) sleep_ms(50);
			std::printf("RAM_SCAN attempt=%u rc=%d total=%u matches=%u channel=%u rssi=%d\n",
			    attempt, started, total, matches, channel, best);
			sleep_ms(3000);
		}
		cyw43_arch_deinit();
	}
	std::puts("RAM_SCAN_DONE b=BOOTSEL");
	for(;;)
	{
		if(getchar_timeout_us(0) == 'b') reset_usb_boot(0, 0);
		sleep_ms(10);
	}
}
