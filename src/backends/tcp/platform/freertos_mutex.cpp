// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/detail/synchronization.h>
#include <FreeRTOS.h>
#include <semphr.h>
#if defined(__GLIBCXX__)
#include <ext/concurrence.h>
#endif

namespace NaWServer::detail
{

#if defined(__GLIBCXX__)
	static_assert(__gnu_cxx::__default_lock_policy == __gnu_cxx::_S_atomic,
	    "Concurrent shared_ptr ownership requires the tested GNU atomic thread model");
#endif

	static_assert(sizeof(StaticSemaphore_t) <= NAWSERVER_MUTEX_STORAGE_BYTES,
	    "Increase NAWSERVER_MUTEX_STORAGE_BYTES for this FreeRTOS port");
	static_assert(alignof(StaticSemaphore_t) <= alignof(std::max_align_t),
	    "FreeRTOS mutex alignment exceeds the private storage contract");
	static_assert(configSUPPORT_STATIC_ALLOCATION && configUSE_MUTEXES,
	    "NaWServer requires statically allocated FreeRTOS mutexes");

	Mutex::Mutex() noexcept
	{
		handle_ = xSemaphoreCreateMutexStatic(reinterpret_cast<StaticSemaphore_t *>(storage_));
		configASSERT(handle_);
	}

	Mutex::~Mutex()
	{
		vSemaphoreDelete(static_cast<SemaphoreHandle_t>(handle_));
	}

	void Mutex::lock()
	{
		const auto result = xSemaphoreTake(static_cast<SemaphoreHandle_t>(handle_), portMAX_DELAY);
		configASSERT(result == pdTRUE);
		(void)result;
	}

	bool Mutex::try_lock()
	{
		return xSemaphoreTake(static_cast<SemaphoreHandle_t>(handle_), 0) == pdTRUE;
	}

	void Mutex::unlock()
	{
		const auto result = xSemaphoreGive(static_cast<SemaphoreHandle_t>(handle_));
		configASSERT(result == pdTRUE);
		(void)result;
	}

} // namespace NaWServer::detail
