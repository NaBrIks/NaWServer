// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "device_details.hpp"
#include "lcd.hpp"
#include "../../common/device_details.hpp"
namespace c6_demo
{
	void encode_device_details(Writer &writer)
	{
		// Board package is ESP32-C6FH4; IDF 5.4 omits EMB_FLASH for C6.
		demo_info::encode_device_details(
			writer,
			[](Writer &writer)
		{
			writer.map(13);
			writer.put("lcd_controller", "ST7789T");
			writer.put("lcd_resolution", "172 x 320");
			writer.put("lcd_spi_ready", lcd_ready());
			writer.put("lcd_visual_status", "unverified");
			writer.put("backlight_percent", 30);
			writer.put("rgb_count", 1);
			writer.put("rgb_gpio", 8);
			writer.put("rgb_order", "RGB");
			writer.put("rgb_driver", "RMT");
			writer.put("sd_slot", true);
			writer.put("sd_mounted", false);
			writer.put("touch", false);
			writer.put("ble_started", false);
			writer.end();
		},
			1);
	}

} // namespace c6_demo
