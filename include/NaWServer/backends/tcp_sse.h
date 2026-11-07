// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/detail/sse/queued_adapter.hpp>
#include <atomic>
#include <NaWServer/detail/synchronization.h>
#include <NaWServer/detail/worker_signal.h>

namespace NaWServer
{

	class TcpHttpBackend;

	namespace tcp::detail
	{
		struct Worker;
	}

	namespace sse
	{

		// Explicit SSE scheduler for one borrowed TCP HTTP backend. start may precede
		// HTTP begin; stop producers and drain this adapter before stopping HTTP.
		class TcpAdapter final : public QueuedAdapter
		{
			TcpHttpBackend &backend_;
			NaWServer::detail::Mutex lifecycle_mutex_;
			tcp::detail::Worker *worker_ = nullptr;
			tcp::detail::WorkerSignal signal_;
			std::atomic<bool> stop_requested_{false};
			static void entry(void *context);
			void run();
			void wakeRuntime() override;
			std::uint64_t runtimeMilliseconds() const override;

		public:
			explicit TcpAdapter(TcpHttpBackend &backend, Limits limits = {});
			~TcpAdapter() override;
			TcpAdapter(const TcpAdapter &) = delete;
			TcpAdapter &operator=(const TcpAdapter &) = delete;
			bool start() override;
			bool stop(std::uint32_t timeout_ms) override;
			Admit reserve(Request &request, const Frame &preface, Reservation &reservation) override;
			std::size_t close_errors() const override;
		};

	} // namespace sse

} // namespace NaWServer
