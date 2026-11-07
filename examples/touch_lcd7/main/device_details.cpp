// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "device_details.hpp"
#include "runtime.hpp"
#include "application.hpp"
#include "../../common/device_details.hpp"
namespace lcd7_web
{
	void encode_device_details(Writer &writer)
	{
		demo_info::encode_device_details(
			writer,
			[](Writer &writer)
		{
			const auto state = runtime_state();
			const auto display = touch_lcd7::snapshot();
			writer.map(14);
			writer.put("lcd_interface", "RGB565");
			writer.put("lcd_resolution", "800 x 480");
			writer.put("lcd_ready", display.ready);
			writer.put("backlight_enabled", display.backlight);
			writer.put("touch", true);
			writer.put("touch_controller", "GT911");
			writer.put("renderer", "LVGL 8.3.11");
			writer.put("framebuffers", 2);
			writer.put("rgb_count", 10);
			writer.put("rgb_gpio", 6);
			writer.put("rgb_order", "GRB");
			writer.put("rgb_driver", state.strip.driver == 1 ? "SPI" : "RMT");
			writer.put("rgb_ready", state.strip.supported);
			writer.put("ble_started", false);
			writer.end();
		});
	}

} // namespace lcd7_web
