// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#endif
#include "backends/tcp/platform/worker.h"
#include <NaWServer/detail/worker_signal.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>

using namespace NaWServer::tcp::detail;

namespace
{

	std::atomic<unsigned> checks{0};

	void check(bool value, int line)
	{
		checks.fetch_add(1);
		if(!value)
		{
			std::printf("TCP_WORKER_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	class Gate
	{
		std::mutex mutex_;
		std::condition_variable condition_;
		bool signaled_ = false;
	public:
		void signal()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			signaled_ = true;
			condition_.notify_all();
		}

		bool wait()
		{
			std::unique_lock<std::mutex> lock(mutex_);
			return condition_.wait_for(lock, std::chrono::seconds(3), [this] {
				return signaled_;
			});
		}

	};

	struct Context
	{
		Gate entered, inspect, inspected, release;
		Worker *worker = nullptr;
		std::atomic<unsigned> calls{0};
		std::atomic<unsigned> returned{0};
		std::atomic<std::size_t> stack_bytes{0};
	};

	std::size_t stackBytes()
	{
#if defined(_WIN32)
		char marker;
		MEMORY_BASIC_INFORMATION memory{};
		CHECK(VirtualQuery(&marker, &memory, sizeof(memory)) == sizeof(memory));
		const auto allocation = memory.AllocationBase;
		auto address = reinterpret_cast<std::uintptr_t>(allocation);
		std::size_t total = 0;
		for(unsigned region = 0; region < 64; ++region)
		{
			CHECK(VirtualQuery(reinterpret_cast<const void *>(address), &memory, sizeof(memory)) == sizeof(memory));
			if(memory.AllocationBase != allocation) return total;

			CHECK(memory.RegionSize != 0);
			total += memory.RegionSize;
			address += memory.RegionSize;
		}
		CHECK(false);
		return 0;
#elif defined(__linux__)
		pthread_attr_t attributes;
		CHECK(pthread_getattr_np(pthread_self(), &attributes) == 0);
		void *address = nullptr;
		std::size_t size = 0;
		CHECK(pthread_attr_getstack(&attributes, &address, &size) == 0);
		CHECK(pthread_attr_destroy(&attributes) == 0);
		CHECK(address && size);
		return size;
#else
		return 0;
#endif
	}

	void controlledEntry(void *argument)
	{
		auto &context = *static_cast<Context *>(argument);
		// This identity check needs no caller-published context.worker pointer.
		CHECK(isTcpWorkerContext(&context));
		CHECK(!isTcpWorkerContext(nullptr));
		CHECK(!isTcpWorkerThread(nullptr));
		context.calls.fetch_add(1);
		context.stack_bytes.store(stackBytes());
		context.entered.signal();
		CHECK(context.inspect.wait());
		CHECK(isTcpWorkerThread(context.worker));
		CHECK(!joinTcpWorker(context.worker, 1));
		context.inspected.signal();
		CHECK(context.release.wait());
		context.returned.fetch_add(1);
	}

	void timeoutAndRetry()
	{
		CHECK(!isTcpWorkerThread(nullptr));
		CHECK(!isTcpWorkerContext(nullptr));
		CHECK(!joinTcpWorker(nullptr, 1));
		CHECK(startTcpWorker(nullptr, nullptr) == nullptr);
		Context context;
		context.worker = startTcpWorker(controlledEntry, &context);
		CHECK(context.worker != nullptr);
		CHECK(context.entered.wait());
		CHECK(!isTcpWorkerThread(context.worker));
		CHECK(!isTcpWorkerContext(&context));
		context.inspect.signal();
		CHECK(context.inspected.wait());
		CHECK(!joinTcpWorker(context.worker, 0));
		const auto tick_before = tcpMilliseconds();
		const auto before = std::chrono::steady_clock::now();
		CHECK(!joinTcpWorker(context.worker, 20));
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - before).count();
		const auto tick_elapsed = tcpMilliseconds() - tick_before;
		CHECK(elapsed >= 1 && elapsed < 1000);
		CHECK(context.calls.load() == 1 && context.returned.load() == 0);
		CHECK(context.stack_bytes.load() != 0);
		context.release.signal();
		CHECK(joinTcpWorker(context.worker, 3000));
		context.worker = nullptr;
		CHECK(!isTcpWorkerContext(&context));
		CHECK(context.returned.load() == 1 && context.calls.load() == 1);
		std::printf("TCP_WORKER native_stack_reported_bytes=%zu timeout_wall_ms=%lld timeout_tick_ms=%llu\n",
		    context.stack_bytes.load(), static_cast<long long>(elapsed),
		    static_cast<unsigned long long>(tick_elapsed));
	}

	struct ImmediateContext
	{
		std::atomic<unsigned> calls{0};
		int unrelated_identity = 0;
	};

	void immediateEntry(void *argument)
	{
		auto &context = *static_cast<ImmediateContext *>(argument);
		CHECK(isTcpWorkerContext(&context));
		CHECK(!isTcpWorkerContext(&context.unrelated_identity));
		context.calls.fetch_add(1);
	}

	void repeatedJoin()
	{
		ImmediateContext context;
		for(unsigned cycle = 0; cycle < 100; ++cycle)
		{
			const auto role = cycle % 2 == 0 ? WorkerRole::Http : WorkerRole::EventStream;
			Worker *worker = startTcpWorker(immediateEntry, &context, role);
			CHECK(worker != nullptr);
			CHECK(joinTcpWorker(worker, 3000));
			CHECK(!isTcpWorkerContext(&context));
			CHECK(context.calls.load() == cycle + 1);
		}
		CHECK(context.calls.load() == 100);
	}

	void nullContextEntry(void *)
	{
		CHECK(isTcpWorkerContext(nullptr));
	}

	struct SignalContext
	{
		WorkerSignal signal;
		Gate entered;
		std::atomic<bool> returned{false};
	};

	void signalEntry(void *argument)
	{
		auto &context = *static_cast<SignalContext *>(argument);
		context.entered.signal();
		CHECK(context.signal.wait(UINT32_MAX));
		context.returned.store(true);
	}

	void latchedSignal()
	{
		SignalContext context;
		CHECK(!context.signal.wait(0));
		context.signal.notify(); // Initialization remains explicit, after SDK startup.
		CHECK(context.signal.initialize() && context.signal.initialize());
		CHECK(!context.signal.wait(0));
		context.signal.notify();
		context.signal.notify();
		CHECK(context.signal.wait(0)); // Notify-before-wait is retained; duplicates coalesce.
		CHECK(!context.signal.wait(0));
		const auto before = std::chrono::steady_clock::now();
		CHECK(!context.signal.wait(20));
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::steady_clock::now() - before).count();
		CHECK(elapsed >= 10 && elapsed < 1000);
		auto *worker = startTcpWorker(signalEntry, &context);
		CHECK(worker && context.entered.wait());
		CHECK(!joinTcpWorker(worker, 0));
		context.signal.notify(); // A shutdown wakeup must release an indefinite waiter.
		CHECK(joinTcpWorker(worker, 3000));
		CHECK(context.returned.load());
	}

} // namespace

int main()
{
	timeoutAndRetry();
	latchedSignal();
	repeatedJoin();
	Worker *worker = startTcpWorker(nullContextEntry, nullptr);
	CHECK(worker != nullptr);
	CHECK(joinTcpWorker(worker, 3000));
	CHECK(!isTcpWorkerContext(nullptr));
	std::printf("TCP_WORKER checks=%u restart_cycles=100 PASS\n", checks.load());
}
