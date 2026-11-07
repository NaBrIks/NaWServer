// SPDX-License-Identifier: MIT
// GNU libstdc++ thread-model bridge for the tested bare Arm toolchain.
// Atomic shared_ptr reference counts also require __GTHREADS and active_p=1.
// NaWServer mutexes use detail::Mutex directly; this is not a full C++ threading port.
#ifndef _GLIBCXX_GCC_GTHR_H
#define _GLIBCXX_GCC_GTHR_H
#define __GTHREADS 1
#define __GTHREAD_MUTEX_INIT { {}, nullptr }
#define __GTHREAD_RECURSIVE_MUTEX_INIT { {}, nullptr }
#define __GTHREAD_ONCE_INIT 0
#define __GTHREAD_COND_INIT {0}
#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>
#include <errno.h>
#include <time.h>
struct __gthread_mutex_t
{
	StaticSemaphore_t storage;
	SemaphoreHandle_t handle;
};
using __gthread_recursive_mutex_t = __gthread_mutex_t;
using __gthread_once_t = int;
using __gthread_time_t = timespec;
struct __gthread_cond_t { unsigned unsupported; };
inline int __gthread_active_p() { return 1; }
inline void nawserver_freertos_mutex_initialize(__gthread_mutex_t *mutex, bool recursive)
{
	if(mutex->handle) return;
	const bool running = xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
	if(running) taskENTER_CRITICAL();
	if(!mutex->handle)
	{
		mutex->handle = recursive ? xSemaphoreCreateRecursiveMutexStatic(&mutex->storage)
		    : xSemaphoreCreateMutexStatic(&mutex->storage);
		configASSERT(mutex->handle);
	}
	if(running) taskEXIT_CRITICAL();
}
inline int __gthread_mutex_lock(__gthread_mutex_t *mutex)
{
	nawserver_freertos_mutex_initialize(mutex, false);
	return xSemaphoreTake(mutex->handle, portMAX_DELAY) == pdTRUE ? 0 : EBUSY;
}
inline int __gthread_mutex_trylock(__gthread_mutex_t *mutex)
{
	nawserver_freertos_mutex_initialize(mutex, false);
	return xSemaphoreTake(mutex->handle, 0) == pdTRUE ? 0 : EBUSY;
}
inline int __gthread_mutex_unlock(__gthread_mutex_t *mutex)
{
	return xSemaphoreGive(mutex->handle) == pdTRUE ? 0 : EPERM;
}
inline int __gthread_mutex_destroy(__gthread_mutex_t *mutex)
{
	if(mutex->handle) vSemaphoreDelete(mutex->handle);
	mutex->handle = nullptr;
	return 0;
}
inline int __gthread_recursive_mutex_lock(__gthread_recursive_mutex_t *mutex)
{
	nawserver_freertos_mutex_initialize(mutex, true);
	return xSemaphoreTakeRecursive(mutex->handle, portMAX_DELAY) == pdTRUE ? 0 : EBUSY;
}
inline int __gthread_recursive_mutex_trylock(__gthread_recursive_mutex_t *mutex)
{
	nawserver_freertos_mutex_initialize(mutex, true);
	return xSemaphoreTakeRecursive(mutex->handle, 0) == pdTRUE ? 0 : EBUSY;
}
inline int __gthread_recursive_mutex_unlock(__gthread_recursive_mutex_t *mutex)
{
	return xSemaphoreGiveRecursive(mutex->handle) == pdTRUE ? 0 : EPERM;
}
inline int __gthread_recursive_mutex_destroy(__gthread_recursive_mutex_t *mutex)
{
	return __gthread_mutex_destroy(mutex);
}
inline int __gthread_yield() { taskYIELD(); return 0; }
// Deliberately undefined: an unexpected use must fail at link time rather than
// pretending that this demo supplies condition variables or std::call_once.
int __gthread_once(__gthread_once_t *, void (*)());
int __gthread_mutex_timedlock(__gthread_mutex_t *, const __gthread_time_t *);
int __gthread_recursive_mutex_timedlock(__gthread_recursive_mutex_t *, const __gthread_time_t *);
int __gthread_cond_destroy(__gthread_cond_t *);
int __gthread_cond_wait(__gthread_cond_t *, __gthread_mutex_t *);
int __gthread_cond_timedwait(__gthread_cond_t *, __gthread_mutex_t *, const __gthread_time_t *);
int __gthread_cond_signal(__gthread_cond_t *);
int __gthread_cond_broadcast(__gthread_cond_t *);
#endif
