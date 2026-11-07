// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/backends/esp_idf_memory.h>
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include <algorithm>
#include <cstdint>

// App-owned telemetry. Linker boundaries describe this image, not a size estimate.
extern "C" {
extern char _data_start[], _data_end[], _bss_start[], _bss_end[];
extern char _demo_memory_main_data_start[], _demo_memory_main_data_end[];
extern char _demo_memory_main_bss_start[], _demo_memory_main_bss_end[];
extern char _demo_memory_library_data_start[], _demo_memory_library_data_end[];
extern char _demo_memory_library_bss_start[], _demo_memory_library_bss_end[];
extern char _demo_memory_main_legacy_start[], _demo_memory_main_legacy_end[];
extern char _demo_memory_main_common_start[], _demo_memory_main_common_end[];
extern char _demo_memory_main_dram_start[], _demo_memory_main_dram_end[];
extern char _demo_memory_library_legacy_start[], _demo_memory_library_legacy_end[];
extern char _demo_memory_library_common_start[], _demo_memory_library_common_end[];
extern char _demo_memory_library_dram_start[], _demo_memory_library_dram_end[];
#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY || CONFIG_ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY
extern char _ext_ram_bss_start[], _ext_ram_bss_end[];
#endif
}
namespace demo_memory
{
	inline std::size_t span(const char *start, const char *end)
	{
		return reinterpret_cast<std::uintptr_t>(end) - reinterpret_cast<std::uintptr_t>(start);
	}

	template <class Writer> void encode_region(Writer &writer, const char *id, unsigned caps,
	    std::size_t static_bytes, std::size_t own_static, std::size_t own_dynamic)
	{
		const auto total = heap_caps_get_total_size(caps);
		const auto free = std::min(heap_caps_get_free_size(caps), total);
		// Allocation/free operations continue while sampling; never emit negatives.
		own_dynamic = std::min(own_dynamic, total - free);
		writer.map(7);
		writer.put("id", id);
		writer.put("total_bytes", static_bytes + total);
		writer.put("free_bytes", free);
		writer.put("own_static_bytes", std::min(own_static, static_bytes));
		writer.put("other_static_bytes", static_bytes - std::min(own_static, static_bytes));
		writer.put("own_dynamic_bytes", own_dynamic);
		writer.put("other_dynamic_bytes", total - free - own_dynamic);
		writer.end();
	}

	template <class Writer> void encode(Writer &writer)
	{
		const auto usage = NaWServer::espIdfBufferUsage();
		const auto own_static = span(_demo_memory_main_data_start, _demo_memory_main_data_end) +
		    span(_demo_memory_main_bss_start, _demo_memory_main_bss_end) +
		    span(_demo_memory_library_data_start, _demo_memory_library_data_end) +
		    span(_demo_memory_library_bss_start, _demo_memory_library_bss_end) +
		    span(_demo_memory_main_legacy_start, _demo_memory_main_legacy_end) +
		    span(_demo_memory_main_common_start, _demo_memory_main_common_end) +
		    span(_demo_memory_main_dram_start, _demo_memory_main_dram_end) +
		    span(_demo_memory_library_legacy_start, _demo_memory_library_legacy_end) +
		    span(_demo_memory_library_common_start, _demo_memory_library_common_end) +
		    span(_demo_memory_library_dram_start, _demo_memory_library_dram_end);
		writer.key("memory_usage");
#if CONFIG_SPIRAM
		writer.array(2);
#else
		writer.array(1);
#endif
		encode_region(writer, "internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
		    span(_data_start, _data_end) + span(_bss_start, _bss_end), own_static, usage.internal_bytes);
#if CONFIG_SPIRAM
		std::size_t external_static = 0;
#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY || CONFIG_ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY
		external_static = span(_ext_ram_bss_start, _ext_ram_bss_end);
#endif
		encode_region(writer, "psram", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
		    external_static, 0, usage.psram_bytes);
#endif
		writer.end();
	}

}
