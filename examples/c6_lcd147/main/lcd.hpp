// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include "esp_err.h"
namespace c6_demo
{
	esp_err_t lcd_start();
	bool lcd_ready();
} // namespace c6_demo
