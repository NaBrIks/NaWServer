#include <FreeRTOS.h>
#include <task.h>
#include <cstdio>
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/time.h"
#include "pico/platform.h"
#include "pico/mutex.h"
#include <reent.h>

namespace
{
	// Newlib's default retarget locks are empty in the tested Arm toolchain.
	// Its own recursive hook also serializes mallinfo and allocations invoked
	// internally by libc, outside Pico's outer malloc/free wrappers.
	auto_init_recursive_mutex(newlib_mutex);
}

extern "C" void __malloc_lock(struct _reent *)
{
	recursive_mutex_enter_blocking(&newlib_mutex);
}

extern "C" void __malloc_unlock(struct _reent *)
{
	recursive_mutex_exit(&newlib_mutex);
}

extern "C" void compact_rp_set_newlib_reent(struct _reent *value)
{
	if(get_core_num() == 0) _impure_ptr = value;
}
extern "C" uint64_t compact_rp_runtime_counter()
{
	return time_us_64();
}
extern "C" void compact_rp_assert_failed(const char *file, int line)
{
	printf("RTOS ASSERT %s:%d\n", file, line);
	watchdog_reboot(0, 0, 100);
	for(;;) {}
}
extern "C" void vApplicationMallocFailedHook()
{
	compact_rp_assert_failed("FreeRTOS heap exhausted", 0);
}
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *name)
{
	compact_rp_assert_failed(name, 0);
}
