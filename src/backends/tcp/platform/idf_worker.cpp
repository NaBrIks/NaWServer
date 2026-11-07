// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "worker.h"
#include "sdkconfig.h"
#include <atomic>
#include <limits>
#include <new>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

namespace NaWServer::tcp::detail
{

	struct Worker
	{
		TaskHandle_t native_handle = nullptr;
		void (*entry)(void *) = nullptr;
		void *context = nullptr;
		std::atomic<bool> completed{false};
	};

	namespace
	{

		thread_local const Worker *current_worker = nullptr;
		thread_local const void *current_context = nullptr;

		void runTcpWorker(void *argument)
		{
			auto *worker = static_cast<Worker *>(argument);
			current_worker = worker;
			current_context = worker->context;
			worker->entry(worker->context);
			// Only this library-owned task's lazily allocated lwIP semaphore is
			// released here. Application tasks and their TLS remain untouched.
			lwip_socket_thread_cleanup();
			current_context = nullptr;
			current_worker = nullptr;
			worker->completed.store(true, std::memory_order_release);
			// No Worker/context access after publication. Suspension may publish
			// its list membership before the final cross-core context switch.
			for(;;)
				vTaskSuspend(nullptr);
		}

		bool quiescent(const Worker &worker)
		{
			if(!worker.completed.load(std::memory_order_acquire) ||
			    eTaskGetState(worker.native_handle) != eSuspended)
				return false;

			// Public SDK queries close the SMP suspended-before-switch window.
			// The opaque private handle is never resumed or exposed externally,
			// so a suspended task cannot migrate back onto a previously checked core.
			for(BaseType_t core = 0; core < configNUMBER_OF_CORES; ++core)
				if(xTaskGetCurrentTaskHandleForCore(core) == worker.native_handle)
					return false;

			return true;
		}

	} // namespace

	Worker *startTcpWorker(void (*entry)(void *), void *context, WorkerRole role) noexcept
	{
		if(!entry || (role != WorkerRole::Http && role != WorkerRole::EventStream && role != WorkerRole::WebSocket &&
            role != WorkerRole::Transmission))
			return nullptr;

		auto *worker = new(std::nothrow) Worker;
		if(!worker) return nullptr;

		worker->entry = entry;
		worker->context = context;
		// ESP-IDF xTaskCreate takes bytes, unlike upstream FreeRTOS stack words.
		// Application callbacks share this stack; the S3 INFO handler exceeds 6 KiB.
		const std::uint32_t stack_bytes = role == WorkerRole::Http ? CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE : 4096;
		const UBaseType_t priority = role == WorkerRole::EventStream ? 4 : 5;
		const char *name = role == WorkerRole::Http ? "naw_tcp_http" :
		    role == WorkerRole::WebSocket ? "naw_tcp_ws" :
            role == WorkerRole::Transmission ? "naw_tcp_tx" : "naw_tcp_sse";
		if(xTaskCreate(runTcpWorker, name, stack_bytes, worker, priority, &worker->native_handle) != pdPASS)
		{
			delete worker;
			return nullptr;
		}

		// Entry/TLS identity reads only fields published before xTaskCreate,
		// never native_handle while the SDK is still publishing that field.
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

		const auto started = tcpMilliseconds();
		for(;;)
		{
			if(quiescent(*worker))
			{
				// Non-running deletion performs TCB/TLS cleanup synchronously.
				// Keep Worker and its borrowed context alive through that cleanup.
				vTaskDelete(worker->native_handle);
				delete worker;
				return true;
			}

			if(tcpMilliseconds() - started >= timeout_ms) return false;

			vTaskDelay(1);
		}
	}

	std::uint64_t tcpMilliseconds() noexcept
	{
		return static_cast<std::uint64_t>(esp_timer_get_time()) / 1000;
	}

	void sleepTcpWorker(std::uint32_t milliseconds) noexcept
	{
		if(milliseconds == 0)
		{
			taskYIELD();
			return;
		}

		const auto ticks = (static_cast<std::uint64_t>(milliseconds) * configTICK_RATE_HZ + 999) / 1000;
		const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<TickType_t>::max()) - 1;
		vTaskDelay(static_cast<TickType_t>(ticks < maximum ? ticks : maximum));
	}

} // namespace NaWServer::tcp::detail
