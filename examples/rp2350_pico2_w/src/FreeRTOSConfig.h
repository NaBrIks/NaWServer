#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C"
#endif
void rp_demo_assert_failed(const char *file, int line);
#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 1
#define configUSE_TICKLESS_IDLE 0
#define configCPU_CLOCK_HZ 150000000
#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 8
#define configMINIMAL_STACK_SIZE 256
#define configMAX_TASK_NAME_LEN 20
#define configTOTAL_HEAP_SIZE (192 * 1024)
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configSUPPORT_STATIC_ALLOCATION 1
#define configKERNEL_PROVIDED_STATIC_MEMORY 1
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configQUEUE_REGISTRY_SIZE 8
#define configUSE_TIMERS 1
#define configTIMER_TASK_PRIORITY 2
#define configTIMER_QUEUE_LENGTH 10
#define configTIMER_TASK_STACK_DEPTH 512
#define configTIMER_SERVICE_TASK_CORE_AFFINITY 1
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configUSE_MALLOC_FAILED_HOOK 1
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configUSE_NEWLIB_REENTRANT 1
struct _reent;
#ifdef __cplusplus
extern "C"
#endif
void rp_demo_set_newlib_reent(struct _reent *value);
// SDK/libc tasks stay on core0. Core1 executes the pure CPU workload and
// SDK RAM-only flash lockout; it must not overwrite newlib's global pointer.
#define configSET_TLS_BLOCK(value) rp_demo_set_newlib_reent(&(value))
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 4
#define configNUMBER_OF_CORES 2
#define configUSE_CORE_AFFINITY 1
#define configIDLE_AFFINITY 1
#define configTASK_DEFAULT_CORE_AFFINITY 1
#define configRUN_MULTIPLE_PRIORITIES 1
#define configUSE_PASSIVE_IDLE_HOOK 0
#define configUSE_TRACE_FACILITY 1
#define configGENERATE_RUN_TIME_STATS 1
#define configRUN_TIME_COUNTER_TYPE uint64_t
#ifdef __cplusplus
extern "C"
#endif
uint64_t rp_demo_runtime_counter(void);
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() ((void)0)
#define portGET_RUN_TIME_COUNTER_VALUE() rp_demo_runtime_counter()
#define configUSE_16_BIT_TICKS 0
#define configENABLE_MPU 0
#define configENABLE_TRUSTZONE 0
#define configRUN_FREERTOS_SECURE_ONLY 1
#define configENABLE_FPU 1
#define configENABLE_MVE 0
#define configMAX_SYSCALL_INTERRUPT_PRIORITY 16
#define configASSERT(value) do { if(!(value)) rp_demo_assert_failed(__FILE__, __LINE__); } while(0)
#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_eTaskGetState 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetIdleTaskHandle 1
#define INCLUDE_xTimerPendFunctionCall 1
#define INCLUDE_xSemaphoreGetMutexHolder 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_vTaskPrioritySet 1
