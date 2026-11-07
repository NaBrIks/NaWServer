// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <cstdint>
#include "esp_err.h"
namespace showcase
{
	struct Experiment
	{
		bool running = false, active = false;
		bool pulse = false;
		int duration = 5000, on = 200, period = 1000, core = 0, priority = 2;
		int64_t start = 0, elapsed = 0;
		double pi = 0;
		uint64_t iterations = 0;
	};
	struct Led
	{
		bool supported = false, enabled = false, level = false;
		int period = 1000, duty = 50;
	};
	struct Strip
	{
		bool supported = false, enabled = true;
		int mode = 0, color = 0xffa030, brightness = 20, period = 1000;
		int driver = 0, requested_driver = 0;
		esp_err_t driver_error = ESP_OK;
		uint32_t pixels[10]{};
	};
	struct State
	{
		Experiment experiment;
		Led led;
		Strip strip;
	};
	struct ExperimentCommand
	{
		bool stop = false, pulse = false;
		int duration = 5000, on = 200, period = 1000, core = 0, priority = 2;
	};
	esp_err_t runtime_init();
	State runtime_state();
	bool set_experiment(const ExperimentCommand &command);
	bool set_led(bool enabled, int period, int duty);
	bool set_strip(bool enabled, int mode, int color, int brightness, int period, int driver = -1);
	// Shared pure color function also exercised by host C++ tests.
	void strip_pixels(Strip &strip, int64_t milliseconds);
} // namespace showcase
