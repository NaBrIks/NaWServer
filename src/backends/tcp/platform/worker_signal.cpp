// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include <NaWServer/detail/worker_signal.h>
#include <new>

#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#elif defined(NAWSERVER_FREERTOS_MUTEX) && NAWSERVER_FREERTOS_MUTEX
#include <FreeRTOS.h>
#include <semphr.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <pthread.h>
#include <time.h>
#endif

namespace NaWServer::tcp::detail
{

#if defined(ESP_PLATFORM) || (defined(NAWSERVER_FREERTOS_MUTEX) && NAWSERVER_FREERTOS_MUTEX)
	struct WorkerSignal::State
	{
		StaticSemaphore_t storage;
		SemaphoreHandle_t handle = nullptr;
	};
#elif defined(_WIN32)
	struct WorkerSignal::State
	{
		SRWLOCK mutex = SRWLOCK_INIT;
		CONDITION_VARIABLE condition = CONDITION_VARIABLE_INIT;
		bool pending = false;
	};
#else
	struct WorkerSignal::State
	{
		pthread_mutex_t mutex;
		pthread_cond_t condition;
		bool pending = false;
	};
#endif

	bool WorkerSignal::initialize() noexcept
	{
		if(initialized_) return true;

		static_assert(sizeof(State) <= sizeof(storage_), "WorkerSignal opaque storage is too small");
		static_assert(alignof(State) <= alignof(std::max_align_t), "WorkerSignal requires stronger alignment");
		auto &state = *new(storage_) State{};
#if defined(ESP_PLATFORM) || (defined(NAWSERVER_FREERTOS_MUTEX) && NAWSERVER_FREERTOS_MUTEX)
		state.handle = xSemaphoreCreateBinaryStatic(&state.storage);
		if(!state.handle) return false;
#elif !defined(_WIN32)
		if(pthread_mutex_init(&state.mutex, nullptr)) return false;

		pthread_condattr_t attributes;
		if(pthread_condattr_init(&attributes)) { pthread_mutex_destroy(&state.mutex); return false; }

		const auto clock_result = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
		const auto condition_result = clock_result ? clock_result : pthread_cond_init(&state.condition, &attributes);
		pthread_condattr_destroy(&attributes);
		if(condition_result) { pthread_mutex_destroy(&state.mutex); return false; }
#else
		(void)state;
#endif
		initialized_ = true;
		return true;
	}

	WorkerSignal::~WorkerSignal()
	{
		if(!initialized_) return;

		auto &state = *reinterpret_cast<State *>(storage_);
#if defined(ESP_PLATFORM) || (defined(NAWSERVER_FREERTOS_MUTEX) && NAWSERVER_FREERTOS_MUTEX)
		vSemaphoreDelete(state.handle);
#elif !defined(_WIN32)
		pthread_cond_destroy(&state.condition);
		pthread_mutex_destroy(&state.mutex);
#else
		(void)state;
#endif
		state.~State();
	}

	void WorkerSignal::notify() noexcept
	{
		if(!initialized_) return;

		auto &state = *reinterpret_cast<State *>(storage_);
#if defined(ESP_PLATFORM) || (defined(NAWSERVER_FREERTOS_MUTEX) && NAWSERVER_FREERTOS_MUTEX)
		(void)xSemaphoreGive(state.handle);
#elif defined(_WIN32)
		AcquireSRWLockExclusive(&state.mutex);
		state.pending = true;
		WakeConditionVariable(&state.condition);
		ReleaseSRWLockExclusive(&state.mutex);
#else
		pthread_mutex_lock(&state.mutex);
		state.pending = true;
		pthread_cond_signal(&state.condition);
		pthread_mutex_unlock(&state.mutex);
#endif
	}

	bool WorkerSignal::wait(std::uint32_t timeout_ms) noexcept
	{
		if(!initialized_) return false;

		auto &state = *reinterpret_cast<State *>(storage_);
#if defined(ESP_PLATFORM) || (defined(NAWSERVER_FREERTOS_MUTEX) && NAWSERVER_FREERTOS_MUTEX)
		const auto ticks = (static_cast<std::uint64_t>(timeout_ms) * configTICK_RATE_HZ + 999) / 1000;
		const auto maximum = static_cast<std::uint64_t>(portMAX_DELAY) - 1;
		const auto interval = timeout_ms == UINT32_MAX ? portMAX_DELAY :
		    static_cast<TickType_t>(ticks < maximum ? ticks : maximum);
		return xSemaphoreTake(state.handle, interval) == pdTRUE;
#elif defined(_WIN32)
		AcquireSRWLockExclusive(&state.mutex);
		const auto started = GetTickCount64();
		while(!state.pending)
		{
			const auto elapsed = GetTickCount64() - started;
			if(timeout_ms != UINT32_MAX && elapsed >= timeout_ms) break;

			const auto interval = timeout_ms == UINT32_MAX ? INFINITE : static_cast<DWORD>(timeout_ms - elapsed);
			if(!SleepConditionVariableSRW(&state.condition, &state.mutex, interval, 0) &&
			    GetLastError() != ERROR_TIMEOUT) break;
		}
		const bool signaled = state.pending;
		state.pending = false;
		ReleaseSRWLockExclusive(&state.mutex);
		return signaled;
#else
		timespec deadline{};
		if(timeout_ms != UINT32_MAX)
		{
			if(clock_gettime(CLOCK_MONOTONIC, &deadline)) return false;

			deadline.tv_sec += timeout_ms / 1000;
			deadline.tv_nsec += static_cast<long>(timeout_ms % 1000) * 1000000;
			deadline.tv_sec += deadline.tv_nsec / 1000000000;
			deadline.tv_nsec %= 1000000000;
		}
		pthread_mutex_lock(&state.mutex);
		while(!state.pending)
		{
			if(!timeout_ms) break;

			const int result = timeout_ms == UINT32_MAX ? pthread_cond_wait(&state.condition, &state.mutex) :
			    pthread_cond_timedwait(&state.condition, &state.mutex, &deadline);
			if(result && result != EINTR) break;
		}
		const bool signaled = state.pending;
		state.pending = false;
		pthread_mutex_unlock(&state.mutex);
		return signaled;
#endif
	}

} // namespace NaWServer::tcp::detail
