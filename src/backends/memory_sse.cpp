// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/memory_sse.h>
#include <algorithm>
#include <limits>
#include <new>

namespace NaWServer
{
	void MemorySseStream::activate()
	{
		active_ = true;
		open_ = true;
	}

	void MemorySseStream::rollback()
	{
		if(active_ || open_) return;

		++rollback_count_;
	}

	int MemorySseStream::write(const char *bytes, std::size_t size)
	{
		if(!open_ || !active_ || (!bytes && size) || fail_writes_) return -1;

		if(!size) return 0;

		if(!max_write_) return 0;

		if(size > capacity_ - std::min(capacity_, bytes_.size())) return -1;

		const auto written = std::min(size, max_write_);
		try
		{
			bytes_.insert(bytes_.end(), bytes, bytes + written);
		}
		catch(const std::bad_alloc &)
		{
			return -1;
		}
		return static_cast<int>(written);
	}

	void MemorySseStream::finish()
	{
		if(!open_) return;

		open_ = false;
		active_ = false;
		++finish_count_;
	}

	std::vector<char> MemorySseStream::take()
	{
		std::vector<char> result;
		result.swap(bytes_);
		return result;
	}

}

namespace NaWServer::sse
{
	bool MemoryAdapter::start()
	{
		return beginRuntime();
	}

	bool MemoryAdapter::stop(std::uint32_t timeout_ms)
	{
		requestStop();
		std::uint64_t elapsed = 0;
		for(;;)
		{
			poll(time_);
			if(finishRuntime()) return true;

			if(elapsed >= timeout_ms) return false;

			if(time_ != std::numeric_limits<std::uint64_t>::max()) ++time_;

			++elapsed;
		}
	}

	void MemoryAdapter::advance(std::uint64_t milliseconds)
	{
		if(milliseconds > std::numeric_limits<std::uint64_t>::max() - time_)
			time_ = std::numeric_limits<std::uint64_t>::max();
		else
			time_ += milliseconds;

		poll(time_);
	}

}
