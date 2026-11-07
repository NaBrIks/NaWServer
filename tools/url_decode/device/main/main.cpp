// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "decoder.hpp"
#include "utf8_fixtures.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_clk_tree.h"
#include "soc/clk_tree_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace urlbench;
static volatile bool tracking = false;
static volatile uint32_t allocations = 0, frees = 0, requested = 0;
extern "C" void esp_heap_trace_alloc_hook(void *, size_t size, uint32_t)
{
	if(tracking)
	{
		++allocations;
		requested += size;
	}
}

extern "C" void esp_heap_trace_free_hook(void *)
{
	if(tracking)
		++frees;
}

static volatile uint32_t sink = 0;
// 64 KiB of varied flash rodata, read through volatile to pressure the unified cache.
constexpr std::array<uint8_t, 65536> pressure_data()
{
	std::array<uint8_t, 65536> a{};
	uint32_t x = 0xc3192026;
	for(auto &b : a)
	{
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		b = uint8_t(x);
	}
	return a;
}

static const auto pressure = pressure_data();
static void pressure_cache()
{
	const volatile uint8_t *p = pressure.data();
	uint32_t s = 0;
	for(size_t i = 0; i < pressure.size(); i += 16)
		s += p[i];
	sink = s;
}

struct Case
{
	const char *name;
	std::string input;
	Mode mode;
};
static std::vector<Case> cases()
{
	std::vector<Case> c = {
		{"tiny_plain", "mode=on", Mode::form},
		{"short_plain", "temperature=23.5", Mode::form},
		{"short_form", "name=Ivan+Petrov&city=%D0%A3%D1%84%D0%B0", Mode::form},
		{"short_path", "/a+b/%D1%82%2Ffile", Mode::path}};
	std::string plain(1024, 'a'), sparse = plain, dense, plus(1024, '+');
	for(size_t i = 61; i + 3 < sparse.size(); i += 64)
		sparse.replace(i, 3, "%2F");
	for(unsigned i = 0; i < 512; ++i)
		dense += "%41";
	auto invalid = sparse;
	invalid.replace(900, 3, "%G1");
	c.push_back({"long_plain", plain, Mode::form});
	c.push_back({"long_path", plain, Mode::path});
	c.push_back({"long_sparse", sparse, Mode::form});
	c.push_back({"long_dense", dense, Mode::form});
	c.push_back({"long_plus", plus, Mode::form});
	c.push_back({"invalid_late", invalid, Mode::form});
	c.push_back({"binary_nul", std::string("A%00B\0C%FF", 10), Mode::form});
	for(const auto &f : unicode_fixtures())
	{
		c.push_back({f.raw_name, f.raw, Mode::form});
		c.push_back({f.percent_name, f.percent, Mode::form});
	}
	return c;
}

__attribute__((noinline)) static Result overhead(const char *, size_t n, char *, size_t, Mode)
{
	asm volatile ("" ::: "memory");
	return {Status::ok, n, n};
}

static constexpr Variant measured[] =
{{"overhead", overhead}, {"direct", direct}, {"lookup", lookup}, {"from_chars", standard}, {"spans", spans}};
static bool unicode_validation()
{
	for(const auto &f : unicode_fixtures())
	{
		for(unsigned representation = 0; representation < 2; ++representation)
		{
			const auto &input = representation ? f.percent : f.raw;
			const char *name = representation ? f.percent_name : f.raw_name;
			unsigned passed = 0;
			for(size_t i = 1; i < std::size(measured); ++i)
			{
				std::vector<char> buffer(input.size() + 4, char(0x5a));
				std::memcpy(buffer.data(), input.data(), input.size());
				std::memset(buffer.data() + input.size(), 0, 3);
				auto r = measured[i].decode(buffer.data(), input.size(), buffer.data(), f.raw.size(), Mode::form);
				const bool good = r.status == Status::ok && r.written == f.raw.size() &&
				    !std::memcmp(buffer.data(), f.raw.data(), f.raw.size()) && buffer.back() == 0x5a;
				if(!good)
				{
					std::printf("ABORT correctness unicode %s %s\n", name, measured[i].name);
					return false;
				}

				++passed;
			}
			std::printf(
				"UTF8_CHECK,%s,%u,%u,%lu,%u\n",
				name,
				unsigned(input.size()),
				unsigned(f.raw.size()),
				(unsigned long)byte_checksum(f.raw),
				passed);
		}
		vTaskDelay(1);
	}
	return true;
}

static char work[4096];
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
struct Sample
{
	uint32_t cycles;
	int64_t us;
	uint32_t alloc, free, bytes;
	int heap_delta;
};
static Sample sample(const Case &c, Decoder fn, unsigned count, bool cold, bool owning)
{
	const auto n = c.input.size();
	// Refresh the selected call path immediately before a warm sample; serial
	// formatting between records must not turn its first call into a cold one.
	if(!cold)
	{
		if(owning)
		{
			std::string output(n, '\0');
			auto r = fn(c.input.data(), n, output.data(), n, c.mode);
			sink += uint32_t(r.written);
			asm volatile ("" : : "r" (output.data()) : "memory");
		}
		else
		{
			std::memcpy(work, c.input.data(), n);
			std::memset(work + n, 0, 4);
			auto r = fn(work, n, work, n, c.mode);
			sink += uint32_t(r.written);
			asm volatile ("" : : "r" (work) : "memory");
		}
	}

	// Heap before/after, not a peak measurement; hooks below count allocation traffic.
	const size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
	portENTER_CRITICAL(&mux);
	if(cold)
		pressure_cache();

	allocations = frees = requested = 0;
	tracking = true;
	const auto t0 = esp_timer_get_time();
	const uint32_t c0 = esp_cpu_get_cycle_count();
	uint32_t result = 0;
	for(unsigned i = 0; i < count; ++i)
	{
		if(owning)
		{
			std::string output(n, '\0');
			auto r = fn(c.input.data(), n, output.data(), n, c.mode);
			result += uint32_t(r.written) + uint32_t(r.status);
			if(r.written)
				result += uint8_t(output[0]) + uint8_t(output[r.written - 1]);

			asm volatile ("" : : "r" (output.data()) : "memory");
		}
		else
		{
			// Same input ownership/reset/capacity for all four candidates.
			std::memcpy(work, c.input.data(), n);
			std::memset(work + n, 0, 4);
			auto r = fn(work, n, work, n, c.mode);
			result += uint32_t(r.written) + uint32_t(r.status);
			if(r.written)
				result += uint8_t(work[0]) + uint8_t(work[r.written - 1]);

			asm volatile ("" : : "r" (work) : "memory");
		}
	}
	const uint32_t elapsed = esp_cpu_get_cycle_count() - c0;
	const auto us = esp_timer_get_time() - t0;
	tracking = false;
	const uint32_t a = allocations, f = frees, b = requested;
	sink += result;
	portEXIT_CRITICAL(&mux);
	return {elapsed, us, a, f, b, int(heap_before) - int(heap_caps_get_free_size(MALLOC_CAP_8BIT))};
}

static void measurements(unsigned run, const std::vector<Case> &data)
{
	for(unsigned style = 0; style < 3; ++style)
	{
		const char *experiment = style == 0 ? "warm" : style == 1 ? "pressure" : "owning";
		const unsigned count = style == 1 ? 1 : 32, repeats = style == 1 ? 15 : 9;
		for(const auto &c : data)
		{
			for(const auto &v : measured)
			{
				if(style == 2 && v.decode == overhead)
					continue;

				for(unsigned warm = 0; warm < 8; ++warm)
					sample(c, v.decode, 1, false, style == 2);
			}
			for(unsigned rep = 0; rep < repeats; ++rep)
			{
				for(unsigned offset = 0; offset < std::size(measured); ++offset)
				{
					const auto &v = measured[(offset + rep + run) % std::size(measured)];
					if(style == 2 && v.decode == overhead)
						continue;

					const auto r = sample(c, v.decode, count, style == 1, style == 2);
					// No serial calls occur inside sample's timed/critical section.
					std::printf(
						"CSV,%u,%s,%s,%s,%u,%u,%u,%lu,%lld,%lu,%lu,%lu,%d\n",
						run,
						experiment,
						c.name,
						v.name,
						rep,
						unsigned(c.input.size()),
						count,
						(unsigned long)r.cycles,
						(long long)r.us,
						(unsigned long)r.alloc,
						(unsigned long)r.free,
						(unsigned long)r.bytes,
						r.heap_delta);
				}
				vTaskDelay(1);
			}
		}
	}
}

static void memory_profiles(const std::vector<Case> &data)
{
	for(const auto &c : data)
		for(const auto &v : variants)
		{
			const size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
			size_t during = before;
			allocations = 0;
			frees = 0;
			requested = 0;
			tracking = true;
			{
				std::string output(c.input.size(), '\0');
				during = heap_caps_get_free_size(MALLOC_CAP_8BIT);
				auto r = v.decode(c.input.data(), c.input.size(), output.data(), output.size(), c.mode);
				sink += uint32_t(r.written);
			}
			tracking = false;
			const size_t after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
			std::printf(
				"MEMORY,%s,%s,%u,%lu,%lu,%lu,%d,%d\n",
				c.name,
				v.name,
				unsigned(c.input.size()),
				(unsigned long)allocations,
				(unsigned long)frees,
				(unsigned long)requested,
				int(before) - int(during),
				int(before) - int(after));
		}
}

extern "C" void app_main()
{
	vTaskDelay(pdMS_TO_TICKS(7000));
	uint32_t cpu = 0;
	esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &cpu);
	const auto *app = esp_app_get_description();
	std::printf(
		"META idf=%s compiler=%s cplusplus=%lu cpu=%lu task_stack=%u tick_hz=%u wifi=off ble=off "
		"logging=outside-measurement optimization=Os\n",
		esp_get_idf_version(),
		__VERSION__,
		(unsigned long)__cplusplus,
		(unsigned long)cpu,
		CONFIG_ESP_MAIN_TASK_STACK_SIZE,
		CONFIG_FREERTOS_HZ);
	static_assert(__cplusplus == 201703L, "Benchmark must use C++17");
	std::printf("APP_ELF_SHA256 ");
	for(auto b : app->app_elf_sha256)
		std::printf("%02x", b);
	std::printf("\n");
	if(correctness())
	{
		std::printf("ABORT correctness\n");
		return;
	}

	if(!unicode_validation())
		return;

	auto data = cases();
	memory_profiles(data);
	std::printf(
		"HEADER,run,experiment,dataset,variant,rep,input_bytes,operations,cycles,microseconds,allocations,frees,"
		"requested_bytes,heap_delta\n");
	for(unsigned run = 0; run < 3; ++run)
	{
		measurements(run, data);
		vTaskDelay(pdMS_TO_TICKS(300));
	}
	uint32_t lo = UINT32_MAX, hi = 0;
	int64_t tu = 0;
	for(unsigned i = 0; i < 101; ++i)
	{
		auto t = esp_timer_get_time();
		auto a = esp_cpu_get_cycle_count();
		auto b = esp_cpu_get_cycle_count();
		tu += esp_timer_get_time() - t;
		lo = std::min(lo, b - a);
		hi = std::max(hi, b - a);
	}
	std::printf(
		"DONE checksum=%lu heap=%u min_heap=%u stack_watermark_bytes=%u cycle_read_min=%lu cycle_read_max=%lu "
		"timer_pair_total_us=%lld\n",
		(unsigned long)sink,
		unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
		unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
		unsigned(uxTaskGetStackHighWaterMark(nullptr)),
		(unsigned long)lo,
		(unsigned long)hi,
		(long long)tu);
	for(;;)
		vTaskDelay(pdMS_TO_TICKS(1000));
}
