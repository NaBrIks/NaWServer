// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once
#include <cstddef>
#include <cstdint>

namespace NaWServer::tcp::detail
{

	// Private single-consumer wakeup, not an application synchronization API.
	// Notifications are latched and may coalesce: the owner's state is authoritative.
	// Initialize after SDK startup; stop/join the waiter before destruction.
	class WorkerSignal
	{
		struct State;
		alignas(std::max_align_t) unsigned char storage_[128]{};
		bool initialized_ = false;
	public:
		WorkerSignal() noexcept = default;
		~WorkerSignal();
		WorkerSignal(const WorkerSignal &) = delete;
		WorkerSignal &operator=(const WorkerSignal &) = delete;
		bool initialize() noexcept;
		void notify() noexcept;
		// UINT32_MAX waits indefinitely; zero only consumes an existing notification.
		bool wait(std::uint32_t timeout_ms) noexcept;
	};

} // namespace NaWServer::tcp::detail
