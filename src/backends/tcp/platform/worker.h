// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <cstdint>

namespace NaWServer::tcp::detail
{

	// Private execution glue for the TCP backend, not an application task API.
	struct Worker;
	enum class WorkerRole { Http, EventStream, WebSocket, Transmission };

	// Starts one host worker using the executable's Windows stack reservation or
	// Linux pthread default stack. IDF uses role-specific byte budgets; host
	// reservations are independent of those MCU task stack budgets.
	// Entry/context are borrowed until successful join; constructors start nothing.
	Worker *startTcpWorker(void (*entry)(void *), void *context, WorkerRole role = WorkerRole::Http) noexcept;
	bool isTcpWorkerThread(const Worker *worker) noexcept;
	// Works before startTcpWorker returns/publishes its Worker pointer to Runtime.
	bool isTcpWorkerContext(const void *context) noexcept;
	// Lifecycle calls are serialized by the owner. Current-worker join is rejected.
	// Timeout/error retains the worker and borrowed context for an explicit retry.
	// Success consumes the pointer; no entry/callback can execute afterward.
	bool joinTcpWorker(Worker *worker, std::uint32_t timeout_ms) noexcept;
	std::uint64_t tcpMilliseconds() noexcept;
	// Private scheduler sleep. Positive durations stay positive on every platform.
	void sleepTcpWorker(std::uint32_t milliseconds) noexcept;

} // namespace NaWServer::tcp::detail
