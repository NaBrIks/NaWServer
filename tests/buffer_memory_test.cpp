// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/buffer_memory.h>
#include <NaWServer/detail/sse/session.hpp>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{
	unsigned checks = 0;
	void check(bool value, int line)
	{
		++checks;
		if(!value) { std::printf("BUFFER_MEMORY_FAIL line=%d\n", line); std::abort(); }
	}

	struct Allocator
	{
		std::size_t calls = 0, releases = 0;
		bool fail = false;
		NaWServer::BufferAllocator resource()
		{
			return {this, [](void *context, std::size_t bytes) -> void *
					{
						auto &self = *static_cast<Allocator *>(context);
						++self.calls;
						return self.fail ? nullptr : std::malloc(bytes);
					}, [](void *context, void *pointer, std::size_t)
					{
						++static_cast<Allocator *>(context)->releases;
						std::free(pointer);
					}};
		}

	};
}
#define CHECK(value) check((value), __LINE__)

int main()
{
	using namespace NaWServer;
	Allocator allocator;
	{
		detail::BufferPool pool;
		CHECK(pool.initialize({allocator.resource(), 32}, BufferGroup::Tcp));
		detail::BufferArray<unsigned char> first(pool, 24), over(pool, 9);
		CHECK(first && !over && allocator.calls == 1);
		CHECK(pool.stats().used_bytes == 24 && pool.stats().budget_rejections == 1);
		first.reset();
		CHECK(pool.stats().used_bytes == 0 && pool.stats().peak_bytes == 24);
		allocator.fail = true;
		detail::BufferArray<unsigned char> failure(pool, 8);
		CHECK(!failure && pool.stats().used_bytes == 0 && pool.stats().allocation_failures == 1);
		allocator.fail = false;
		detail::BufferArray<unsigned char> exact(pool, 32);
		CHECK(exact && pool.stats().peak_bytes == 32);
	}
	CHECK(allocator.releases == 2);
	{
		alignas(std::max_align_t) unsigned char unaligned[64];
		bool released = false;
		struct Context { unsigned char *storage; bool *released; } context{unaligned, &released};
		BufferAllocator invalid{&context, [](void *value, std::size_t) -> void *
								{
									return static_cast<Context *>(value)->storage + 1;
								}, [](void *value, void *, std::size_t) {
									*static_cast<Context *>(value)->released = true;
								}};
		detail::BufferPool pool;
		CHECK(pool.initialize({invalid, 32}, BufferGroup::Tcp));
		detail::BufferArray<unsigned char> rejected(pool, 16);
		CHECK(!rejected && released && pool.stats().used_bytes == 0);
		CHECK(pool.stats().allocation_failures == 1);
	}
	{
		// A retained disconnected handle must not depend on a dead budget owner.
		detail::BufferArray<unsigned char> retained;
		{
			detail::BufferPool pool;
			CHECK(pool.initialize({allocator.resource(), 16}, BufferGroup::Tcp));
			CHECK(retained.allocate(pool, 16));
		}
		retained[0] = 42;
		retained.reset();
		CHECK(allocator.releases == 3);
	}
	{
		detail::BufferPool pool;
		CHECK(!pool.initialize({{}, 0}, BufferGroup::Tcp));
		auto invalid = allocator.resource(); invalid.release = nullptr;
		CHECK(!pool.initialize({invalid, 100}, BufferGroup::Tcp));
	}
	{
		detail::BufferPool pool;
		CHECK(pool.initialize({{}, 128}, BufferGroup::Tcp));
		std::atomic<bool> exceeded{false};
		std::vector<std::thread> workers;
		for(unsigned index = 0; index < 8; ++index)
			workers.emplace_back([&]
			{
				for(unsigned iteration = 0; iteration < 1000; ++iteration)
				{
					detail::BufferArray<unsigned char> buffer(pool, 32);
					if(pool.stats().used_bytes > 128) exceeded = true;
				}
			});
		for(auto &worker : workers)
			worker.join();
		CHECK(!exceeded && pool.stats().used_bytes == 0 && pool.stats().peak_bytes <= 128);
	}
	{
		sse::Limits limits;
		limits.buffer_memory = {allocator.resource(), 1};
		sse::Sessions sessions(limits);
		const auto calls = allocator.calls;
		CHECK(!sessions.prepareStorage());
		CHECK(allocator.calls == calls && sessions.bufferMemoryStats().budget_rejections == 1);
	}
	{
		sse::Limits limits;
		limits.clients = 1;
		limits.buffer_memory.allocator = allocator.resource();
		sse::Sessions sessions(limits);
		CHECK(sessions.prepareStorage());
		const auto one_client_bytes = sessions.bufferMemoryStats().used_bytes;
		CHECK(one_client_bytes > 0 && one_client_bytes < sizeof(sse::Frame));
		CHECK(sessions.managedMemoryStats().allocated_blocks == 0);
		const auto token = sessions.reserve(7, sse::default_header(), 0);
		CHECK(token.slot < sse::client_capacity && sessions.managedMemoryStats().active_blocks == 1);
		CHECK(sessions.bufferMemoryStats().used_bytes ==
		    one_client_bytes + sizeof(sse::Frame) * (sse::pending_capacity + 1));
		CHECK(sessions.rollback(token));
		CHECK(sessions.bufferMemoryStats().used_bytes == one_client_bytes);
		sessions.releaseStorage();
		CHECK(sessions.bufferMemoryStats().used_bytes == 0);
		CHECK(sessions.prepareStorage());
		CHECK(sessions.bufferMemoryStats().used_bytes == one_client_bytes);
	}
	std::printf("BUFFER_MEMORY_PASS checks=%u\n", checks);
}
