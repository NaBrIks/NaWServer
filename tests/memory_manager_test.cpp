// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/memory_manager.h>
#include <array>
#include <cstdio>
#include <cstdlib>
using namespace NaWServer;
namespace
{
	void check(bool value, int line)
	{
		if(!value) { std::printf("MEMORY_MANAGER_FAIL %d\n", line); std::abort(); }
	}

#define CHECK(value) check(static_cast<bool>(value), __LINE__)
	struct Heap
	{
		std::size_t free = 100000, calls = 0, live = 0;
		bool probe_ok = true;
		static void *allocate(void *context, std::size_t bytes)
		{
			auto &heap = *static_cast<Heap *>(context);
			++heap.calls;
			auto *pointer = std::malloc(bytes);
			if(pointer) { heap.free -= bytes; ++heap.live; }

			return pointer;
		}

		static void release(void *context, void *pointer, std::size_t bytes)
		{
			auto &heap = *static_cast<Heap *>(context);
			std::free(pointer);
			heap.free += bytes;
			--heap.live;
		}

		static bool available(void *context, BufferAvailability &result)
		{
			const auto &heap = *static_cast<Heap *>(context);
			result = {heap.free, heap.free};
			return heap.probe_ok;
		}

	};
}
int main()
{
	Heap heap;
	{
		detail::BufferPool parent;
		CHECK(parent.initialize({{&heap, Heap::allocate, Heap::release, Heap::available}, 8192, 1000},
		    BufferGroup::Tcp));
		MemoryManager manager;
		CHECK(manager.initialize(parent, {2, 9, 256}));
		CHECK(manager.stats().allocated_blocks == 2 && manager.stats().allocated_bytes == 512);
		std::array<MemoryBlock, 9> blocks;
		blocks[0] = manager.acquire(20);
		blocks[1] = manager.acquire(30);
		const auto calls = heap.calls;
		heap.free = 1001;
		CHECK(manager.readiness(64) == MemoryBlockReason::LowMemory);
		CHECK(!manager.acquire(64) && heap.calls == calls);
		CHECK(manager.stats().blocked == MemoryBlockReason::LowMemory);
		heap.free = 100000;
		heap.probe_ok = false;
		CHECK(manager.readiness(64) == MemoryBlockReason::LowMemory);
		CHECK(!manager.acquire(64) && heap.calls == calls);
		blocks[0].reset();
		CHECK(manager.readiness(64) == MemoryBlockReason::Ready);
		blocks[0] = manager.acquire(64);
		CHECK(blocks[0] && heap.calls == calls);
		heap.probe_ok = true;
		heap.free = 100000;
		for(std::size_t index = 2; index < blocks.size(); ++index)
			blocks[index] = manager.acquire(64);
		CHECK(manager.stats().allocated_blocks == 9 && manager.stats().active_blocks == 9);
		CHECK(!manager.acquire(1) && manager.stats().blocked == MemoryBlockReason::SlotLimit);
		for(auto &block : blocks)
			block.reset();
		CHECK(manager.stats().allocated_blocks == 2 && manager.stats().allocated_bytes == 512);
		CHECK(manager.stats().active_blocks == 0);
		manager.reset();
		CHECK(parent.stats().used_bytes == 0);
	}
	CHECK(heap.live == 0);
	std::puts("MEMORY_MANAGER_PASS initial2 maximum9 floor_no_attempt dynamic_release=1");
}
