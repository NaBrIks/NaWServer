// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/detail/sse/queued_adapter.hpp>
#include <NaWServer/sse_backend.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace NaWServer
{
	class MemorySseStream final : public sse::Stream
	{
		std::vector<char> bytes_;
		std::size_t capacity_;
		std::size_t max_write_ = 256;
		bool fail_writes_ = false;
		bool active_ = false;
		bool open_ = false;
		std::size_t finish_count_ = 0;
		std::size_t rollback_count_ = 0;
	public:
		explicit MemorySseStream(std::size_t capacity = 16 * 1024)
		  : capacity_(capacity)
		{
		}

		void activate() override;
		void rollback() override;
		int write(const char *bytes, std::size_t size) override;
		void finish() override;
		bool isOpen() const override
		{
			return open_;
		}

		void setMaxWrite(std::size_t size)
		{
			max_write_ = size;
		}

		void failWrites(bool fail)
		{
			fail_writes_ = fail;
		}

		std::vector<char> take();
		std::size_t finishCount() const
		{
			return finish_count_;
		}

		std::size_t rollbackCount() const
		{
			return rollback_count_;
		}

	};

} // namespace NaWServer

namespace NaWServer::sse
{
	class MemoryAdapter final : public QueuedAdapter
	{
		std::uint64_t time_ = 0;
	public:
		explicit MemoryAdapter(Limits limits = {})
		  : QueuedAdapter(limits)
		{
		}

		bool start() override;
		bool stop(std::uint32_t timeout_ms) override;
		void advance(std::uint64_t milliseconds);
	};
}
