// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "worker.h"
#include <cerrno>
#include <limits>
#include <new>
#include <pthread.h>
#include <time.h>

namespace NaWServer::tcp::detail
{

	struct Worker
	{
		pthread_t native_handle{};
		void (*entry)(void *) = nullptr;
		void *context = nullptr;
	};

	namespace
	{

		thread_local const Worker *current_worker = nullptr;
		thread_local const void *current_context = nullptr;

		void *runTcpWorker(void *argument)
		{
			auto *worker = static_cast<Worker *>(argument);
			current_worker = worker;
			current_context = worker->context;
			worker->entry(worker->context);
			current_context = nullptr;
			current_worker = nullptr;
			return nullptr;
		}

		bool before(const timespec &left, const timespec &right)
		{
			return left.tv_sec < right.tv_sec || (left.tv_sec == right.tv_sec && left.tv_nsec < right.tv_nsec);
		}

		bool deadlineAfter(timespec now, std::uint32_t milliseconds, timespec &deadline)
		{
			const auto seconds = static_cast<time_t>(milliseconds / 1000);
			const auto nanoseconds = now.tv_nsec + static_cast<long>(milliseconds % 1000) * 1000000;
			const auto carry = nanoseconds / 1000000000;
			if(now.tv_sec > std::numeric_limits<time_t>::max() - seconds - carry)
				return false;

			deadline = {now.tv_sec + seconds + carry, nanoseconds % 1000000000};
			return true;
		}

	} // namespace

	Worker *startTcpWorker(void (*entry)(void *), void *context, WorkerRole) noexcept
	{
		if(!entry) return nullptr;

		auto *worker = new(std::nothrow) Worker;
		if(!worker) return nullptr;

		worker->entry = entry;
		worker->context = context;
		if(pthread_create(&worker->native_handle, nullptr, runTcpWorker, worker) != 0)
		{
			delete worker;
			return nullptr;
		}

		// The entry reads only fields published before pthread_create. TLS identity
		// does not read native_handle while pthread_create is publishing it.
		return worker;
	}

	bool isTcpWorkerThread(const Worker *worker) noexcept
	{
		return worker && current_worker == worker;
	}

	bool isTcpWorkerContext(const void *context) noexcept
	{
		return current_worker && current_context == context;
	}

	bool joinTcpWorker(Worker *worker, std::uint32_t timeout_ms) noexcept
	{
		if(!worker || isTcpWorkerThread(worker)) return false;

		timespec now;
		timespec deadline;
		if(clock_gettime(CLOCK_MONOTONIC, &now) != 0 || !deadlineAfter(now, timeout_ms, deadline))
			return false;

		for(;;)
		{
			const int joined = pthread_tryjoin_np(worker->native_handle, nullptr);
			if(joined == 0)
			{
				delete worker;
				return true;
			}

			if(joined != EBUSY) return false;

			if(clock_gettime(CLOCK_MONOTONIC, &now) != 0 || !before(now, deadline)) return false;

			timespec interval;
			if(!deadlineAfter(now, 1, interval)) return false;

			if(before(deadline, interval)) interval = deadline;

			// One short absolute wait. EINTR returns to the deadline check instead
			// of restarting a relative interval or extending the join budget.
			const int slept = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &interval, nullptr);
			if(slept != 0 && slept != EINTR) return false;
		}
	}

	std::uint64_t tcpMilliseconds() noexcept
	{
		timespec now;
		if(clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;

		return static_cast<std::uint64_t>(now.tv_sec) * 1000 + static_cast<std::uint64_t>(now.tv_nsec) / 1000000;
	}

	void sleepTcpWorker(std::uint32_t milliseconds) noexcept
	{
		timespec now;
		timespec deadline;
		if(clock_gettime(CLOCK_MONOTONIC, &now) != 0 || !deadlineAfter(now, milliseconds ? milliseconds : 1, deadline))
			return;

		for(;;)
		{
			const int slept = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
			if(slept != EINTR) return;

			if(clock_gettime(CLOCK_MONOTONIC, &now) != 0 || !before(now, deadline)) return;
		}
	}

} // namespace NaWServer::tcp::detail
