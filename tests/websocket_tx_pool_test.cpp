// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "backends/tcp/websocket_tx_pool.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>
#include <utility>

namespace
{
	std::atomic<std::size_t> live_allocations{0};
	std::size_t allocation_calls = 0;
	bool measure = false;
	int fail_after = -1;
	unsigned checks = 0;
	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("WEBSOCKET_TX_POOL_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

}

void *operator new(std::size_t size)
{
	if(measure)
		++allocation_calls;

	if(void *storage = std::malloc(size ? size : 1))
	{
		live_allocations.fetch_add(1);
		return storage;
	}

	std::abort();
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
	if(fail_after == 0)
	{
		fail_after = -1;
		return nullptr;
	}

	if(fail_after > 0)
		--fail_after;

	return ::operator new(size);
}

void *operator new[](std::size_t size)
{
	return ::operator new(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept
{
	return ::operator new(size, tag);
}

void operator delete(void *storage) noexcept
{
	if(storage)
		live_allocations.fetch_sub(1);

	std::free(storage);
}

void operator delete[](void *storage) noexcept
{
	::operator delete(storage);
}

void operator delete(void *storage, std::size_t) noexcept
{
	::operator delete(storage);
}

void operator delete[](void *storage, std::size_t) noexcept
{
	::operator delete(storage);
}

void operator delete(void *storage, const std::nothrow_t &) noexcept
{
	::operator delete(storage);
}

void operator delete[](void *storage, const std::nothrow_t &) noexcept
{
	::operator delete(storage);
}

namespace NaWServer::tcp::detail
{
	struct WebSocketTxPoolTestAccess
	{
		static std::mutex &mutex(WebSocketTxPool &pool)
		{
			return pool.mutex_;
		}

	};
}

namespace
{
	using namespace NaWServer::tcp::detail;
	using Status = WebSocketTxPool::AcquireStatus;
	#define CHECK(value) check((value), __LINE__)

	void invalidAndAllocationFailure()
	{
		const auto before = live_allocations.load();
		for(auto sizes : {std::pair<std::size_t, std::size_t>{0, 4096}, {4, 0},
						  {4, std::numeric_limits<std::size_t>::max()},
						  {std::numeric_limits<std::size_t>::max(), 1}})
		{
			WebSocketTxPool pool(sizes.first, sizes.second);
			CHECK(!pool.valid() && pool.usedBlocks() == 0);
			CHECK(pool.tryAcquire().status == Status::InvalidState);
			CHECK(live_allocations.load() == before);
		}
		for(int ordinal : {0, 1, 2})
		{
			fail_after = ordinal;
			WebSocketTxPool pool(4, 4096);
			CHECK(!pool.valid() && fail_after == -1);
			CHECK(pool.tryAcquire().status == Status::InvalidState);
			CHECK(live_allocations.load() == before);
		}
	}

	void ownershipAndBound()
	{
		WebSocketTxPool pool(4, 4096);
		CHECK(pool.valid() && pool.blockCount() == 4 && pool.blockSize() == 4096);
		std::array<WebSocketTxPool::Lease, 4> blocks;
		measure = true;
		for(std::size_t index = 0; index < blocks.size(); ++index)
		{
			auto acquired = pool.tryAcquire();
			CHECK(acquired.status == Status::Acquired && acquired.block);
			CHECK(acquired.block.capacity() == 4096);
			blocks[index] = std::move(acquired.block);
			CHECK(!acquired.block && acquired.block.data() == nullptr && acquired.block.capacity() == 0);
			blocks[index].data()[0] = static_cast<std::uint8_t>(index);
			for(std::size_t previous = 0; previous < index; ++previous)
				CHECK(blocks[index].data() != blocks[previous].data());
		}
		CHECK(pool.usedBlocks() == 4 && pool.tryAcquire().status == Status::GlobalFull);
		auto *first_address = blocks[0].data();
		blocks[0] = std::move(blocks[1]); // Releases old first lease before replacing it.
		CHECK(pool.usedBlocks() == 3 && !blocks[1] && blocks[0].data()[0] == 1);
		auto reused = pool.tryAcquire();
		CHECK(reused.status == Status::Acquired && reused.block.data() == first_address);
		auto moved = std::move(reused.block);
		CHECK(!reused.block && pool.usedBlocks() == 4);
		moved.reset();
		moved.reset();
		for(auto &block : blocks)
			block.reset();

		CHECK(pool.usedBlocks() == 0 && allocation_calls == 0);
		measure = false;
	}

	void busyAndConcurrentOwners()
	{
		WebSocketTxPool pool(4, 4096);
		std::mutex state_mutex;
		std::condition_variable condition;
		bool locked = false;
		bool release = false;
		std::thread holder([&]
		    {
		    std::lock_guard<std::mutex> pool_lock(WebSocketTxPoolTestAccess::mutex(pool));
		    std::unique_lock<std::mutex> state_lock(state_mutex);
		    locked = true;
		    condition.notify_one();
		    condition.wait(state_lock, [&] {
		    return release;
				});
			});
		{
			std::unique_lock<std::mutex> state_lock(state_mutex);
			CHECK(condition.wait_for(state_lock, std::chrono::seconds(3), [&] {
				return locked;
			}));
		}
		auto busy = pool.tryAcquire();
		CHECK(busy.status == Status::Busy && !busy.block);
		{
			std::lock_guard<std::mutex> state_lock(state_mutex);
			release = true;
		}
		condition.notify_one();
		holder.join();
		CHECK(pool.usedBlocks() == 0);
		std::atomic<bool> valid{true};
		std::atomic<unsigned> acquired_count{0};
		std::array<std::thread, 8> workers;
		for(unsigned id = 0; id < workers.size(); ++id)
		{
			workers[id] = std::thread([&, id]
			{
				for(unsigned attempt = 0; attempt < 2000; ++attempt)
				{
					auto acquired = pool.tryAcquire();
					if(acquired.status != Status::Acquired)
					{
						if(acquired.status != Status::Busy && acquired.status != Status::GlobalFull)
							valid.store(false);

						continue;
					}

					++acquired_count;
					for(std::size_t byte = 0; byte < acquired.block.capacity(); ++byte)
						acquired.block.data()[byte] = static_cast<std::uint8_t>(id);

					std::this_thread::yield();
					for(std::size_t byte = 0; byte < acquired.block.capacity(); ++byte)
						if(acquired.block.data()[byte] != id)
							valid.store(false);
				}
			});
		}
		for(auto &worker : workers)
			worker.join();

		CHECK(valid.load() && acquired_count.load() > 0 && pool.usedBlocks() == 0);
	}

}

int main()
{
	invalidAndAllocationFailure();
	ownershipAndBound();
	busyAndConcurrentOwners();
	std::printf("WEBSOCKET_TX_POOL_PASS checks=%u pool_bytes=%zu lease_bytes=%zu hot_allocations=%zu\n",
	    checks, sizeof(WebSocketTxPool), sizeof(WebSocketTxPool::Lease), allocation_calls);
}
