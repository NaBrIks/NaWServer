// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "backends/tcp/websocket_tx_pool.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>

namespace
{

	using Pool = NaWServer::tcp::detail::WebSocketTxPool;
	constexpr std::size_t block_count = 4;
	constexpr std::size_t block_size = 4096;
	constexpr std::size_t producers = 8;
	constexpr unsigned iterations = 1000;
	std::atomic<unsigned> checks{0};

	void check(bool value, int line)
	{
		checks.fetch_add(1, std::memory_order_relaxed);
		if(!value)
		{
			std::printf("WEBSOCKET_TX_POOL_CONCURRENCY_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	class StartGate
	{
		std::mutex mutex_;
		std::condition_variable condition_;
		std::size_t waiting_ = 0;
		bool released_ = false;
	public:
		void enter()
		{
			std::unique_lock<std::mutex> lock(mutex_);
			++waiting_;
			condition_.notify_all();
			CHECK(condition_.wait_for(lock, std::chrono::seconds(3), [this] {
				return released_;
			}));
		}

		void release()
		{
			std::unique_lock<std::mutex> lock(mutex_);
			CHECK(condition_.wait_for(lock, std::chrono::seconds(3), [this] {
				return waiting_ == producers;
			}));
			released_ = true;
			condition_.notify_all();
		}

	};

	std::size_t identify(const std::array<std::uint8_t *, block_count> &addresses, const std::uint8_t *bytes)
	{
		for(std::size_t index = 0; index < addresses.size(); ++index)
			if(addresses[index] == bytes) return index;

		CHECK(false);
		return addresses.size();
	}

	std::uint8_t pattern(std::uint64_t stamp, std::size_t offset)
	{
		return static_cast<std::uint8_t>((stamp >> ((offset % 8) * 8)) ^ (offset * 29));
	}

	void fill(Pool::Lease &lease, std::uint64_t stamp)
	{
		for(std::size_t offset = 0; offset < lease.capacity(); ++offset)
			lease.data()[offset] = pattern(stamp, offset);
	}

	bool matches(const Pool::Lease &lease, std::uint64_t stamp)
	{
		for(std::size_t offset = 0; offset < lease.capacity(); ++offset)
			if(lease.data()[offset] != pattern(stamp, offset)) return false;

		return true;
	}

} // namespace

int main()
{
	Pool pool(block_count, block_size);
	CHECK(pool.valid() && pool.blockCount() == block_count && pool.blockSize() == block_size);
	std::array<std::uint8_t *, block_count> addresses{};
	std::array<Pool::Lease, block_count> held;
	for(std::size_t index = 0; index < block_count; ++index)
	{
		auto result = pool.tryAcquire();
		CHECK(result.status == Pool::AcquireStatus::Acquired && result.block.capacity() == block_size);
		addresses[index] = result.block.data();
		CHECK(addresses[index] != nullptr);
		for(std::size_t previous = 0; previous < index; ++previous)
			CHECK(addresses[index] != addresses[previous]);

		held[index] = std::move(result.block);
	}
	CHECK(pool.usedBlocks() == block_count);
	CHECK(pool.tryAcquire().status == Pool::AcquireStatus::GlobalFull);
	for(auto &lease : held)
		lease.reset();

	CHECK(pool.usedBlocks() == 0);
	std::array<std::atomic<unsigned>, block_count> owners;
	for(auto &owner : owners)
		owner.store(0, std::memory_order_relaxed);

	StartGate gate;
	std::atomic<unsigned> accepted{0}, busy{0}, full{0};
	std::array<std::thread, producers> threads;
	for(std::size_t producer = 0; producer < producers; ++producer)
	{
		threads[producer] = std::thread([&, producer]
		{
			gate.enter();
			for(unsigned iteration = 0; iteration < iterations; ++iteration)
			{
				auto result = pool.tryAcquire();
				if(result.status != Pool::AcquireStatus::Acquired)
				{
					CHECK(!result.block && result.block.data() == nullptr && result.block.capacity() == 0);
					if(result.status == Pool::AcquireStatus::Busy) busy.fetch_add(1, std::memory_order_relaxed);
					else
					{
						CHECK(result.status == Pool::AcquireStatus::GlobalFull);
						full.fetch_add(1, std::memory_order_relaxed);
					}

					std::this_thread::yield();
					continue;
				}

				CHECK(result.block && result.block.capacity() == block_size);
				const auto index = identify(addresses, result.block.data());
				unsigned expected = 0;
				// A second grant of a live block fails independently of TSan or the
				// pool's metadata representation, before concurrent payload writes.
				// Relaxed guard operations detect overlap without supplying a test-only
				// happens-before edge. Payload reuse must be synchronized by the pool.
				CHECK(owners[index].compare_exchange_strong(expected, 1, std::memory_order_relaxed));
				const auto stamp = (static_cast<std::uint64_t>(producer + 1) << 32) | (iteration + 1);
				fill(result.block, stamp);
				std::this_thread::yield();
				CHECK(matches(result.block, stamp));

				Pool::Lease active(std::move(result.block));
				CHECK(!result.block && active.data() == addresses[index] && active.capacity() == block_size);
				std::this_thread::yield();
				CHECK(matches(active, stamp));
				Pool::Lease completed;
				completed = std::move(active);
				CHECK(!active && completed.data() == addresses[index] && completed.capacity() == block_size);
				CHECK(matches(completed, stamp));
				// No payload access follows this guard clear. The actual pool block
				// stays occupied until reset releases its single move-only lease.
				CHECK(owners[index].exchange(0, std::memory_order_relaxed) == 1);
				completed.reset();
				CHECK(!completed && completed.data() == nullptr && completed.capacity() == 0);
				accepted.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
	gate.release();
	for(auto &thread : threads)
		thread.join();

	CHECK(accepted.load() > 0);
	CHECK(accepted.load() + busy.load() + full.load() == producers * iterations);
	for(auto &owner : owners)
		CHECK(owner.load() == 0);

	CHECK(pool.usedBlocks() == 0);
	std::array<bool, block_count> seen{};
	for(auto &lease : held)
	{
		auto result = pool.tryAcquire();
		CHECK(result.status == Pool::AcquireStatus::Acquired && result.block.capacity() == block_size);
		const auto index = identify(addresses, result.block.data());
		CHECK(!seen[index]);
		seen[index] = true;
		lease = std::move(result.block);
	}
	CHECK(pool.usedBlocks() == block_count);
	for(auto &lease : held)
		lease.reset();

	CHECK(pool.usedBlocks() == 0);
	std::printf("WEBSOCKET_TX_POOL_CONCURRENCY_PASS checks=%u producers=%zu attempts=%zu accepted=%u busy=%u full=%u\n",
	    checks.load(), producers, producers * iterations, accepted.load(), busy.load(), full.load());
}
