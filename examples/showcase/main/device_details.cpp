// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "device_details.hpp"
#include "runtime.hpp"
#include "../../common/device_details.hpp"
namespace showcase
{
	void encode_device_details(Writer &writer)
	{
		demo_info::encode_device_details(
			writer,
			[](Writer &writer)
		{
			const auto state = runtime_state();
			writer.map(7);
			writer.put("rgb_count", 10);
			writer.put("rgb_gpio", 2);
			writer.put("rgb_order", "GRB");
			writer.put("rgb_driver", state.strip.driver == 1 ? "SPI" : "RMT");
			writer.put("rgb_ready", state.strip.supported);
			writer.put("board_led_ready", state.led.supported);
			writer.put("ble_started", false);
			writer.end();
		});
	}

} // namespace showcase
