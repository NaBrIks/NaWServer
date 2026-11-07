// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/windows_sse.h>
#include <chrono>
#include <cstdlib>

namespace
{
	std::uint64_t monotonicMilliseconds()
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				   std::chrono::steady_clock::now().time_since_epoch()).count());
	}

}

namespace NaWServer::sse
{
	WindowsAdapter::~WindowsAdapter()
	{
		if(worker_.joinable())
		{
			if(!stop(0)) std::abort();
		}
	}

	bool WindowsAdapter::start()
	{
		std::lock_guard<std::mutex> lock(lifecycle_mutex_);
		if(worker_.joinable() || !beginRuntime()) return false;

		stop_requested_ = false;
		worker_done_ = false;
		poll(monotonicMilliseconds());
		try
		{
			worker_ = std::thread(&WindowsAdapter::run, this);
		}
		catch(...)
		{
			requestStop();
			poll(monotonicMilliseconds());
			(void)finishRuntime();
			return false;
		}
		return true;
	}

	bool WindowsAdapter::stop(std::uint32_t timeout_ms)
	{
		std::unique_lock<std::mutex> lock(lifecycle_mutex_);
		if(!worker_.joinable()) return true;

		requestStop();
		stop_requested_ = true;
		finished_.notify_all();
		if(!worker_done_ && !finished_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
		    [this] {
			return worker_done_;
		})) return false;

		worker_.join();
		worker_done_ = false;
		stop_requested_ = false;
		return true;
	}

	void WindowsAdapter::run()
	{
		for(;;)
		{
			poll(monotonicMilliseconds());
			if(stop_requested_.load(std::memory_order_acquire) && finishRuntime()) break;

			std::unique_lock<std::mutex> lock(lifecycle_mutex_);
			finished_.wait_for(lock, std::chrono::milliseconds(1));
		}
		{
			std::lock_guard<std::mutex> lock(lifecycle_mutex_);
			worker_done_ = true;
		}
		finished_.notify_all();
	}

}
