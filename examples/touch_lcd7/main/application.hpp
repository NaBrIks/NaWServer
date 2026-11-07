// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <cstdint>
#include "board.hpp"
namespace touch_lcd7
{
	struct State
	{
		bool ready = false;
		bool backlight = true;
		unsigned pattern = 0;
		unsigned touches = 0;
		unsigned touch_errors = 0;
		GraphicsStats graphics{};
		Touch touch;
	};
	enum class Command : uint8_t
	{
		Bars,
		Grid,
		Red,
		Green,
		Blue,
		BacklightOn,
		BacklightOff
	};
	bool start_application();
	bool submit(Command command);
	State snapshot();
	void start_web();
	void start_network();
} // namespace touch_lcd7
