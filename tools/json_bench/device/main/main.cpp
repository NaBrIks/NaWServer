// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>

extern "C" int run_json_benchmark(std::uint64_t (*now_microseconds)(), std::size_t iterations);

static std::uint64_t microseconds()
{
	return static_cast<std::uint64_t>(esp_timer_get_time());
}

extern "C" void app_main()
{
	// Let the USB serial monitor attach after vendor flash/reset. This bounded
	// acceptance fixture has no Wi-Fi, HTTP clients, LCD or background workload.
	vTaskDelay(pdMS_TO_TICKS(5000));
	std::printf("JSON_BENCH_HEAP before=%u minimum=%u stack_free=%u\n",
	    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
	    static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
	    static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
	const int result = run_json_benchmark(microseconds, 200);
	std::printf("JSON_BENCH_DONE result=%d heap_after=%u heap_minimum=%u stack_free=%u\n", result,
	    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
	    static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
	    static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}
