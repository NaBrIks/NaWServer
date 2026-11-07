// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/buffer_memory.h>

namespace NaWServer
{
	// ESP-IDF only. Strict policies never consume the other kind of RAM.
	enum class EspIdfMemoryPolicy { System, InternalOnly, PsramOnly, PreferInternal, PreferPsram };
	BufferAllocator espIdfBufferAllocator(EspIdfMemoryPolicy policy) noexcept;
	// Live payload bytes allocated by this helper, across TCP/SSE instances.
	// Excludes allocator padding, control objects and allocations outside the helper.
	struct EspIdfBufferUsage { std::size_t internal_bytes = 0, psram_bytes = 0; };
	EspIdfBufferUsage espIdfBufferUsage() noexcept;
}
