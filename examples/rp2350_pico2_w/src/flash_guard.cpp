#include <FreeRTOS.h>
#include <cstdint>
#include <task.h>
#include "pico/flash.h"
#include "pico/time.h"
#include "hardware/sync.h"

namespace
{
	// Storage serializes every operation on CPU0. This permanent CPU1 task
	// replaces the SDK's allocation/deletion of one lockout task per page.
	enum State : std::uint32_t { idle, requested, ready, released, finished };
	volatile State state = idle;
	TaskHandle_t guard_task = nullptr;
	StaticTask_t guard_control;
	StackType_t guard_stack[512];
	std::uint32_t interrupt_state = 0;

	void __not_in_flash_func(guard)(void *)
	{
		for(;;)
		{
			(void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
			const auto saved = save_and_disable_interrupts();
			if(state == requested)
			{
				state = ready;
				__sev();
				while(state == ready) __wfe();
			}
			restore_interrupts(saved);
			state = finished;
			__sev();
		}
	}
	bool initialize(bool)
	{
		return true;
	}
	int enter(std::uint32_t timeout_ms)
	{
		if(get_core_num() != 0) return PICO_ERROR_NOT_PERMITTED;
		if(state != idle && state != finished) return PICO_ERROR_INVALID_STATE;
		if(!guard_task)
		{
			guard_task = xTaskCreateStaticAffinitySet(guard, "flash-guard", 512, nullptr,
				configMAX_PRIORITIES - 1, guard_stack, &guard_control, 2);
			if(!guard_task) return PICO_ERROR_INSUFFICIENT_RESOURCES;
		}
		state = requested;
		__dmb();
		xTaskNotifyGive(guard_task);
		const auto deadline = make_timeout_time_ms(timeout_ms);
		while(state != ready && !time_reached(deadline)) __wfe();
		if(state != ready)
		{
			state = released;
			__sev();
			return PICO_ERROR_TIMEOUT;
		}
		interrupt_state = save_and_disable_interrupts();
		return PICO_OK;
	}
	int leave(std::uint32_t timeout_ms)
	{
		restore_interrupts(interrupt_state);
		state = released;
		__sev();
		const auto deadline = make_timeout_time_ms(timeout_ms);
		while(state != finished && !time_reached(deadline)) __wfe();
		return state == finished ? PICO_OK : PICO_ERROR_TIMEOUT;
	}
	flash_safety_helper_t helper{initialize, enter, leave};
}

extern "C" flash_safety_helper_t *get_flash_safety_helper()
{
	return &helper;
}
