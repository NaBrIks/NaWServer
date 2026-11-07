// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include "esp_lcd_panel_ops.h"
namespace touch_lcd7
{
	esp_err_t initialize_graphics(esp_lcd_panel_handle_t panel);
	// Called only by the LCD worker; empty address means no active IPv4 interface.
	void display_network_address(const char *address);
} // namespace touch_lcd7
