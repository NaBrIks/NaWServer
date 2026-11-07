// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/sse_backend.h>
#include <atomic>
#include <NaWServer/detail/synchronization.h>

namespace NaWServer::sse
{
	// Shared bounded SSE engine for transports exposing an owned Stream. Scheduling
	// belongs to the concrete adapter; poll performs at most one 256-byte step/slot.
	class QueuedAdapter : public Backend
	{
		Sessions sessions_;
		mutable NaWServer::detail::Mutex mutex_;
		std::array<std::shared_ptr<Stream>, client_capacity> streams_{};
		std::array<Token, client_capacity> tokens_{};
		std::array<WriteBuffer, client_capacity> direct_headers_;
		// Keep only close identity while the transport retains its lease. A full
		// Step array would duplicate >1 KiB of payload scratch for four clients.
		std::array<std::uint64_t, client_capacity> close_serials_{};
		std::array<bool, client_capacity> close_pending_{};
		struct AbortSlot { std::atomic<bool> requested{false}; QueuedAdapter *owner = nullptr; };
		std::array<AbortSlot, client_capacity> aborts_{};
		bool started_ = false, stopping_ = false;
		const void *owner_ = nullptr;
		std::uint64_t time_ = 0;
		std::size_t begun_ = 0, completed_ = 0, rolled_back_ = 0;
		std::array<std::size_t, 8> closed_by_reason_{};
		static void abortExternal(void *context);
	protected:
		bool beginRuntime(bool allow_restart = false, bool direct_storage = false);
		void requestStop();
		void poll(std::uint64_t milliseconds);
		std::uint32_t pollDelay(std::uint64_t milliseconds) const;
		virtual void wakeRuntime() {}
		// Poll-driven adapters retain their supplied clock. Sleeping TCP adapters
		// sample their native monotonic clock before a producer stamps new work.
		virtual std::uint64_t runtimeMilliseconds() const { return time_; }
		bool finishRuntime();
	public:
		explicit QueuedAdapter(Limits limits = {})
		  : sessions_(limits)
		{
		}

		~QueuedAdapter() override;
		Admit reserve(Request &, const Frame &, Reservation &) override;
		bool activate(Reservation &) override;
		void rollback(Reservation &) override;
		Publish publish(Token, const Frame &) override;
		bool close(Token) override;
		AdapterStats stats() override;
		BufferMemoryStats bufferMemoryStats() const
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			return sessions_.bufferMemoryStats();
		}

		std::size_t close_errors() const override
		{
			return 0;
		}

	};
}
