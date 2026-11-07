// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/buffer_memory.h>
#include <cstdint>
#include <cstring>
#if defined(ESP_PLATFORM)
#include <NaWServer/backends/esp_idf_memory.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#endif

namespace NaWServer
{
	namespace
	{
		void *systemAllocate(void *, std::size_t bytes)
		{
			return ::operator new(bytes, std::nothrow);
		}

		void systemRelease(void *, void *storage, std::size_t)
		{
			::operator delete(storage);
		}

#if defined(ESP_PLATFORM)
		std::atomic<std::size_t> internal_payload{0}, psram_payload{0};
		void *idfAllocateGuarded(void *context, std::size_t bytes, std::size_t floor)
		{
			// IDF capability malloc guarantees only four-byte alignment. Preserve its
			// preference/failure-hook behavior while satisfying the C++ buffer contract.
			constexpr auto alignment = alignof(std::max_align_t);
			constexpr auto overhead = sizeof(void *) + alignment - 1;
			if(bytes > std::numeric_limits<std::size_t>::max() - overhead) return nullptr;

			const auto requested = bytes + overhead;
			const auto policy = *static_cast<const EspIdfMemoryPolicy *>(context);
			constexpr unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
			constexpr unsigned external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
			void *raw = nullptr;
			const auto attempt = [requested, floor](unsigned caps) -> void *
				{
					const auto free = heap_caps_get_free_size(caps);
					if(floor &&
					    (free < floor || requested > free - floor ||
					    requested > heap_caps_get_largest_free_block(caps)))
						return nullptr;

					return heap_caps_malloc(requested, caps);
				};
			switch(policy)
			{
			case EspIdfMemoryPolicy::System:
				if(!floor) raw = std::malloc(requested);
				else raw = attempt(MALLOC_CAP_8BIT);

				break;
			case EspIdfMemoryPolicy::InternalOnly: raw = attempt(internal); break;
			case EspIdfMemoryPolicy::PsramOnly: raw = attempt(external); break;
			case EspIdfMemoryPolicy::PreferInternal:
				if(!floor) raw = heap_caps_malloc_prefer(requested, 2, internal, external);
				else { raw = attempt(internal); if(!raw) raw = attempt(external);}

				break;
			case EspIdfMemoryPolicy::PreferPsram:
				if(!floor) raw = heap_caps_malloc_prefer(requested, 2, external, internal);
				else { raw = attempt(external); if(!raw) raw = attempt(internal);}

				break;
			}
			if(!raw) return nullptr;

			const auto start = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void *);
			const auto aligned = (start + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
			auto *storage = reinterpret_cast<void *>(aligned);
			std::memcpy(static_cast<char *>(storage) - sizeof(void *), &raw, sizeof(raw));
			(esp_ptr_external_ram(raw) ? psram_payload : internal_payload).fetch_add(bytes);
			return storage;
		}

		void *idfAllocate(void *context, std::size_t bytes)
		{
			return idfAllocateGuarded(context, bytes, 0);
		}

		bool idfAvailable(void *context, BufferAvailability &result)
		{
			const auto policy = *static_cast<const EspIdfMemoryPolicy *>(context);
			const auto sample = [](unsigned caps)
				{
					return BufferAvailability{heap_caps_get_free_size(caps), heap_caps_get_largest_free_block(caps)};
				};
			const auto internal = sample(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
			const auto external = sample(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
			if(policy == EspIdfMemoryPolicy::InternalOnly) result = internal;
			else if(policy == EspIdfMemoryPolicy::PsramOnly) result = external;
			else if(policy == EspIdfMemoryPolicy::System) result = sample(MALLOC_CAP_8BIT);
			else result = internal.free_bytes >= external.free_bytes ? internal : external;

			return true;
		}

		void idfRelease(void *, void *storage, std::size_t bytes)
		{
			void *raw;
			std::memcpy(&raw, static_cast<char *>(storage) - sizeof(void *), sizeof(raw));
			(esp_ptr_external_ram(raw) ? psram_payload : internal_payload).fetch_sub(bytes);
			heap_caps_free(raw);
		}

#endif
	}

#if defined(ESP_PLATFORM)
	EspIdfBufferUsage espIdfBufferUsage() noexcept
	{
		return {internal_payload.load(), psram_payload.load()};
	}

	BufferAllocator espIdfBufferAllocator(EspIdfMemoryPolicy policy) noexcept
	{
		static EspIdfMemoryPolicy policies[]{EspIdfMemoryPolicy::System, EspIdfMemoryPolicy::InternalOnly,
			                                 EspIdfMemoryPolicy::PsramOnly, EspIdfMemoryPolicy::PreferInternal,
			                                 EspIdfMemoryPolicy::PreferPsram};
		const auto index = static_cast<unsigned>(policy);
		if(index >= sizeof(policies) / sizeof(policies[0])) return {};

		return {&policies[index], idfAllocate, idfRelease, idfAvailable, idfAllocateGuarded};
	}

#endif

	BufferAllocator defaultBufferAllocator(BufferGroup group) noexcept
	{
#if defined(ESP_PLATFORM)
		int selection = 0;
#if defined(CONFIG_NAWSERVER_TCP_MEMORY_POLICY)
		if(group == BufferGroup::Tcp) selection = CONFIG_NAWSERVER_TCP_MEMORY_POLICY;

#endif
#if defined(CONFIG_NAWSERVER_SSE_MEMORY_POLICY)
		if(group == BufferGroup::Sse) selection = CONFIG_NAWSERVER_SSE_MEMORY_POLICY;

#endif
		(void)group;
		return espIdfBufferAllocator(static_cast<EspIdfMemoryPolicy>(selection));
#else
		(void)group;
#endif
		return {nullptr, systemAllocate, systemRelease};
	}

	std::size_t defaultBufferLimit(BufferGroup group) noexcept
	{
#if defined(ESP_PLATFORM) && defined(CONFIG_NAWSERVER_TCP_BUFFER_BYTES)
		if(group == BufferGroup::Tcp) return CONFIG_NAWSERVER_TCP_BUFFER_BYTES;

#endif
#if defined(ESP_PLATFORM) && defined(CONFIG_NAWSERVER_SSE_BUFFER_BYTES)
		if(group == BufferGroup::Sse) return CONFIG_NAWSERVER_SSE_BUFFER_BYTES;

#endif
		return group == BufferGroup::Tcp ? 64 * 1024 : 16 * 1024;
	}

}
