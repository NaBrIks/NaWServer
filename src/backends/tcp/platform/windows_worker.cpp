// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#include "worker.h"
#include <new>

namespace NaWServer::tcp::detail
{

	struct Worker
	{
		HANDLE native_handle = nullptr;
		void (*entry)(void *) = nullptr;
		void *context = nullptr;
	};

	namespace
	{

		thread_local const Worker *current_worker = nullptr;
		thread_local const void *current_context = nullptr;

		unsigned __stdcall runTcpWorker(void *argument)
		{
			auto *worker = static_cast<Worker *>(argument);
			current_worker = worker;
			current_context = worker->context;
			worker->entry(worker->context);
			current_context = nullptr;
			current_worker = nullptr;
			return 0;
		}

	} // namespace

	Worker *startTcpWorker(void (*entry)(void *), void *context, WorkerRole) noexcept
	{
		if(!entry)
			return nullptr;

		auto *worker = new(std::nothrow) Worker;
		if(!worker)
			return nullptr;

		worker->entry = entry;
		worker->context = context;
		const auto native_handle = _beginthreadex(nullptr, 0, runTcpWorker, worker, 0, nullptr);
		if(native_handle == 0)
		{
			delete worker;
			return nullptr;
		}

		// The entry reads only fields published before _beginthreadex. Its TLS
		// identity is valid even if it runs before this native handle is stored.
		worker->native_handle = reinterpret_cast<HANDLE>(native_handle);
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
		if(!worker || isTcpWorkerThread(worker))
			return false;

		const auto started = tcpMilliseconds();
		std::uint64_t remaining = timeout_ms;
		for(;;)
		{
			// MAXDWORD is INFINITE in this API. Split that finite requested bound
			// instead of accidentally turning it into an unbounded wait.
			const DWORD interval = remaining >= INFINITE ? INFINITE - 1 : static_cast<DWORD>(remaining);
			const DWORD result = WaitForSingleObject(worker->native_handle, interval);
			if(result == WAIT_OBJECT_0)
			{
				if(!CloseHandle(worker->native_handle))
					return false;

				delete worker;
				return true;
			}

			if(result != WAIT_TIMEOUT)
				return false;

			const auto elapsed = tcpMilliseconds() - started;
			if(elapsed >= timeout_ms)
				return false;

			remaining = timeout_ms - elapsed;
		}
	}

	std::uint64_t tcpMilliseconds() noexcept
	{
		return static_cast<std::uint64_t>(GetTickCount64());
	}

	void sleepTcpWorker(std::uint32_t milliseconds) noexcept
	{
		Sleep(milliseconds ? milliseconds : 1);
	}

} // namespace NaWServer::tcp::detail
