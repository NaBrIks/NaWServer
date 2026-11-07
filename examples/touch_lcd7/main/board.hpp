// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <cstdint>
#include "esp_err.h"
namespace touch_lcd7
{
	struct Touch
	{
		bool pressed = false;
		uint16_t x = 0;
		uint16_t y = 0;
	};
	esp_err_t initialize_board();
	esp_err_t read_touch(Touch &touch);
	esp_err_t set_backlight(bool enabled);
	void draw_screen(unsigned pattern, const Touch &touch, unsigned touches);
	void process_graphics();
	struct GraphicsStats
	{
		uint32_t swaps, frame_events, vsyncs;
	};
	GraphicsStats graphics_stats();
} // namespace touch_lcd7
