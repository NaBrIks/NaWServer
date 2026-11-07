#include <FreeRTOS.h>
#include <task.h>
#include <cstdio>
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/time.h"
#include "pico/platform.h"
#include <reent.h>
extern "C" void rp_demo_set_newlib_reent(struct _reent *value)
{
	if(get_core_num() == 0) _impure_ptr = value;
}
extern "C" uint64_t rp_demo_runtime_counter()
{
	return time_us_64();
}
extern "C" void rp_demo_assert_failed(const char *file, int line)
{
	printf("RTOS ASSERT %s:%d\n", file, line);
	watchdog_reboot(0, 0, 100);
	for(;;) {}
}
extern "C" void vApplicationMallocFailedHook()
{
	rp_demo_assert_failed("FreeRTOS heap exhausted", 0);
}
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *name)
{
	rp_demo_assert_failed(name, 0);
}
