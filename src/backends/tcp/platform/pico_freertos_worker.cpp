// SPDX-License-Identifier: MIT
#include "backends/tcp/platform/worker.h"
#include <FreeRTOS.h>
#include <task.h>
#include <atomic>
#include <limits>
#include <new>
#include "pico/time.h"
#include "pico/platform.h"

#ifndef NAWSERVER_PICO_HTTP_STACK_WORDS
#define NAWSERVER_PICO_HTTP_STACK_WORDS 3072
#endif
#ifndef NAWSERVER_PICO_TX_STACK_WORDS
#define NAWSERVER_PICO_TX_STACK_WORDS 1024
#endif
#ifndef NAWSERVER_PICO_STREAM_STACK_WORDS
#define NAWSERVER_PICO_STREAM_STACK_WORDS 1536
#endif
#if NAWSERVER_PICO_HTTP_STACK_WORDS < 256 || NAWSERVER_PICO_TX_STACK_WORDS < 256 || NAWSERVER_PICO_STREAM_STACK_WORDS < 256
#error Invalid Pico worker stack size
#endif

namespace NaWServer::tcp::detail
{
	static_assert(NAWSERVER_PICO_HTTP_STACK_WORDS <= std::numeric_limits<configSTACK_DEPTH_TYPE>::max() &&
	    NAWSERVER_PICO_TX_STACK_WORDS <= std::numeric_limits<configSTACK_DEPTH_TYPE>::max() &&
	    NAWSERVER_PICO_STREAM_STACK_WORDS <= std::numeric_limits<configSTACK_DEPTH_TYPE>::max(),
	    "Pico worker stack depth exceeds FreeRTOS stack depth type");
	static_assert(NAWSERVER_PICO_HTTP_STACK_WORDS <= std::numeric_limits<std::size_t>::max() / sizeof(StackType_t) &&
	    NAWSERVER_PICO_TX_STACK_WORDS <= std::numeric_limits<std::size_t>::max() / sizeof(StackType_t) &&
	    NAWSERVER_PICO_STREAM_STACK_WORDS <= std::numeric_limits<std::size_t>::max() / sizeof(StackType_t),
	    "Pico worker stack allocation size overflows size_t");
	static_assert(configNUM_THREAD_LOCAL_STORAGE_POINTERS >= 4,
	    "NaWServer reserves FreeRTOS task-local slots 2 and 3");
#if configNUMBER_OF_CORES > 1
	static_assert(configUSE_CORE_AFFINITY,
	    "The supported Pico SMP worker profile requires explicit CPU0 affinity");
#endif
	struct Worker
	{
		TaskHandle_t native_handle = nullptr;
		void (*entry)(void *) = nullptr;
		void *context = nullptr;
		std::atomic<bool> completed{false};
	};

	static void runWorker(void *argument)
	{
		auto *worker = static_cast<Worker *>(argument);
		// Keep slots 0/1 free for SDK/lwIP conventions.
		vTaskSetThreadLocalStoragePointer(nullptr, 2, worker);
		vTaskSetThreadLocalStoragePointer(nullptr, 3, worker->context);
		worker->entry(worker->context);
		vTaskSetThreadLocalStoragePointer(nullptr, 2, nullptr);
		vTaskSetThreadLocalStoragePointer(nullptr, 3, nullptr);
		worker->completed.store(true, std::memory_order_release);
		for(;;) vTaskSuspend(nullptr);
	}

	Worker *startTcpWorker(void (*entry)(void *), void *context, WorkerRole role) noexcept
	{
		if(!entry) return nullptr;
		auto *worker = new(std::nothrow) Worker;
		if(!worker) return nullptr;
		worker->entry = entry;
		worker->context = context;
		// Upstream FreeRTOS takes stack words, unlike ESP-IDF's byte convention.
		const auto depth = role == WorkerRole::Http ? NAWSERVER_PICO_HTTP_STACK_WORDS :
		    role == WorkerRole::Transmission ? NAWSERVER_PICO_TX_STACK_WORDS : NAWSERVER_PICO_STREAM_STACK_WORDS;
		const char *name = role == WorkerRole::Http ? "http" : role == WorkerRole::WebSocket ? "websocket" :
		    role == WorkerRole::Transmission ? "transmission" : "sse";
#if configNUMBER_OF_CORES > 1
		// The tested newlib integration and synchronous suspended-task deletion
		// require CPU0. Do not silently inherit a different application default.
		const auto created = xTaskCreateAffinitySet(runWorker, name, depth, worker, 3, 1, &worker->native_handle);
#else
		const auto created = xTaskCreate(runWorker, name, depth, worker, 3, &worker->native_handle);
#endif
		if(created != pdPASS)
		{
			delete worker;
			return nullptr;
		}
		return worker;
	}

	bool isTcpWorkerThread(const Worker *worker) noexcept
	{
		return worker && pvTaskGetThreadLocalStoragePointer(nullptr, 2) == worker;
	}

	bool isTcpWorkerContext(const void *context) noexcept
	{
		return context && pvTaskGetThreadLocalStoragePointer(nullptr, 3) == context;
	}

	bool joinTcpWorker(Worker *worker, std::uint32_t timeout_ms) noexcept
	{
		if(!worker || isTcpWorkerThread(worker)) return false;
#if configNUMBER_OF_CORES > 1
		if(get_core_num() != 0) return false;
#endif
		const auto started = tcpMilliseconds();
		do
		{
			if(worker->completed.load(std::memory_order_acquire) && eTaskGetState(worker->native_handle) == eSuspended)
			{
				// Server workers and their join callers are all pinned to core0.
				// The suspended worker has therefore completed its context switch.
				vTaskDelete(worker->native_handle);
				delete worker;
				return true;
			}
			vTaskDelay(1);
		} while(tcpMilliseconds() - started < timeout_ms);
		return false;
	}

	std::uint64_t tcpMilliseconds() noexcept
	{
		return time_us_64() / 1000;
	}

	void sleepTcpWorker(std::uint32_t milliseconds) noexcept
	{
		if(!milliseconds) taskYIELD();
		else vTaskDelay(pdMS_TO_TICKS(milliseconds) ? pdMS_TO_TICKS(milliseconds) : 1);
	}
}
