// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/detail/sse/queued_adapter.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace NaWServer::sse
{
	// One bounded 1 ms scheduler for Streams retained by WindowsHttpBackend.
	// Stop publishers, drain successfully, then stop the HTTP backend.
	class WindowsAdapter final : public QueuedAdapter
	{
		std::mutex lifecycle_mutex_;
		std::condition_variable finished_;
		std::thread worker_;
		std::atomic<bool> stop_requested_{false};
		bool worker_done_ = false;
		void run();

	public:
		explicit WindowsAdapter(Limits limits = {})
		  : QueuedAdapter(limits)
		{
		}

		~WindowsAdapter() override;
		WindowsAdapter(const WindowsAdapter &) = delete;
		WindowsAdapter &operator=(const WindowsAdapter &) = delete;
		bool start() override;
		bool stop(std::uint32_t timeout_ms) override;
	};
}
