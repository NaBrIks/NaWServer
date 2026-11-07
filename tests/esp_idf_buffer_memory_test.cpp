// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/esp_idf_memory.h>
#include "esp_heap_caps.h"
#include <cstdarg>
#include <cstdio>

namespace
{
	unsigned internal_calls = 0, external_calls = 0;
	bool internal_available = true, external_available = true;
	void *allocated_raw = nullptr, *allocated_shifted = nullptr;
	void check(bool value, int line)
	{
		if(!value) { std::printf("IDF_BUFFER_MEMORY_FAIL line=%d\n", line); std::abort(); }
	}

}
#define CHECK(value) check((value), __LINE__)
static unsigned allocated_caps = 0;
bool esp_ptr_external_ram(const void *)
{
	return (allocated_caps & MALLOC_CAP_SPIRAM) != 0;
}

void *heap_caps_malloc(std::size_t bytes, unsigned caps)
{
	if(caps & MALLOC_CAP_INTERNAL) { ++internal_calls; if(!internal_available) return nullptr;}

	if(caps & MALLOC_CAP_SPIRAM) { ++external_calls; if(!external_available) return nullptr;}

	// Model IDF's four-byte capability alignment rather than the host malloc's
	// stronger accidental alignment. Only one allocation is live in this fixture.
	allocated_raw = std::malloc(bytes + 4);
	allocated_caps = caps;
	allocated_shifted = allocated_raw ? static_cast<char *>(allocated_raw) + 4 : nullptr;
	return allocated_shifted;
}

void *heap_caps_malloc_prefer(std::size_t bytes, std::size_t count, ...)
{
	va_list arguments;
	va_start(arguments, count);
	void *result = nullptr;
	for(std::size_t index = 0; index < count && !result; ++index)
		result = heap_caps_malloc(bytes, va_arg(arguments, unsigned));
	va_end(arguments);
	return result;
}

void heap_caps_free(void *storage)
{
	CHECK(storage == allocated_shifted);
	std::free(allocated_raw);
	allocated_raw = allocated_shifted = nullptr;
}

std::size_t heap_caps_get_free_size(unsigned caps)
{
	return ((caps & MALLOC_CAP_INTERNAL) ? internal_available : external_available) ? 1024 * 1024 : 0;
}

std::size_t heap_caps_get_largest_free_block(unsigned caps)
{
	return heap_caps_get_free_size(caps);
}

int main()
{
	using namespace NaWServer;
	auto external = espIdfBufferAllocator(EspIdfMemoryPolicy::PsramOnly);
	external_available = false;
	CHECK(!external.allocate(external.context, 64) && internal_calls == 0 && external_calls == 1);
	auto preferred = espIdfBufferAllocator(EspIdfMemoryPolicy::PreferPsram);
	auto *storage = preferred.allocate(preferred.context, 64);
	CHECK(storage && internal_calls == 1 && external_calls == 2);
	CHECK(espIdfBufferUsage().internal_bytes == 64 && espIdfBufferUsage().psram_bytes == 0);
	CHECK(reinterpret_cast<std::uintptr_t>(storage) % alignof(std::max_align_t) == 0);
	preferred.release(preferred.context, storage, 64);
	CHECK(espIdfBufferUsage().internal_bytes == 0 && espIdfBufferUsage().psram_bytes == 0);
	external_available = true;
	internal_available = false;
	auto internal = espIdfBufferAllocator(EspIdfMemoryPolicy::InternalOnly);
	const auto before = external_calls;
	CHECK(!internal.allocate(internal.context, 64) && external_calls == before);
	preferred = espIdfBufferAllocator(EspIdfMemoryPolicy::PreferInternal);
	storage = preferred.allocate(preferred.context, 64);
	CHECK(storage && external_calls == before + 1);
	CHECK(espIdfBufferUsage().internal_bytes == 0 && espIdfBufferUsage().psram_bytes == 64);
	preferred.release(preferred.context, storage, 64);
	CHECK(defaultBufferLimit(BufferGroup::Tcp) == 49152);
	CHECK(defaultBufferLimit(BufferGroup::Sse) == 16384);
	const auto defaults = defaultBufferAllocator(BufferGroup::Sse);
	external_available = false;
	const auto internal_before = internal_calls;
	CHECK(!defaults.allocate(defaults.context, 64) && internal_calls == internal_before);
	CHECK(!espIdfBufferAllocator(static_cast<EspIdfMemoryPolicy>(99)).valid());
	std::printf("IDF_BUFFER_MEMORY_PASS strict_no_fallback=1\n");
}
