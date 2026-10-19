/*
 * FreeRTOS Kernel V11.1.0 配置文件
 * 目标：STM32F407ZGT6（Cortex-M4F，硬浮点）/ GNU Arm GCC / STM32CubeMX + CMake 工程
 *
 * 内核源码：ThirdParty/FreeRTOS/Source（取自 D:\FreeRTOSv202406.05-LTS）
 * 移植层  ：ThirdParty/FreeRTOS/Source/portable/GCC/ARM_CM4F
 * 本文件位置：Core/Inc/FreeRTOSConfig.h（参考工程 D:\平衡车\平衡车程序 同样放这里）
 *
 * 本文件不会被 STM32CubeMX 重新生成（除非在 .ioc 里启用 FreeRTOS 中间件——不要那样做，
 * 会带进第二套内核 V10.3.1 + CMSIS-RTOS2，与本工程冲突）。
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stddef.h>   /* size_t：configSTACK_DEPTH_TYPE 用 */
#include <stdint.h>

/* ===========================================================================
 * 硬件描述
 * ===========================================================================
 * configCPU_CLOCK_HZ 用 CMSIS 的 SystemCoreClock 变量，而不是写死常量 168000000：
 *   - 本工程当前 SystemClock_Config() 用的是 HSI 16 MHz、PLL 关闭，写死 168 MHz 会让
 *     RTOS 的 tick 与实际时间差 10.5 倍；
 *   - 以后在 CubeMX 里改成 HSE+PLL(168 MHz) 时，SystemCoreClock 会被
 *     HAL_RCC_ClockConfig() -> SystemCoreClockUpdate() 自动刷新，配置无需再改。
 * 移植层只在运行期表达式里使用它（port.c 的 vPortSetupTimerInterrupt：
 * portNVIC_SYSTICK_LOAD_REG = (configSYSTICK_CLOCK_HZ / configTICK_RATE_HZ) - 1），
 * 不做预处理判断，因此运行期变量是合法的。
 */
extern uint32_t SystemCoreClock;   /* 由 system_stm32f4xx.c 定义，CMSIS 标准符号 */

#define configCPU_CLOCK_HZ                    ( SystemCoreClock )

/* ===========================================================================
 * 调度行为
 * =========================================================================== */
#define configUSE_PREEMPTION                  1
#define configUSE_TIME_SLICING                1      /* 同优先级任务按 tick 轮转 */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0  /* Cortex-M4 用通用算法即可 */
#define configUSE_TICKLESS_IDLE               0      /* 低功耗 tickless 未启用 */
#define configTICK_RATE_HZ                    ( 1000 )  /* 1 ms 一个 tick */
#define configMAX_PRIORITIES                  8
#define configMINIMAL_STACK_SIZE              ( 128 )   /* 单位：字（4 字节） */
#define configMAX_TASK_NAME_LEN               16
#define configTICK_TYPE_WIDTH_IN_BITS         TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD               1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1
#define configQUEUE_REGISTRY_SIZE             0
#define configENABLE_BACKWARD_COMPATIBILITY   0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0
#define configUSE_MINI_LIST_ITEM              1
#define configSTACK_DEPTH_TYPE                size_t
#define configMESSAGE_BUFFER_LENGTH_TYPE      size_t
#define configHEAP_CLEAR_MEMORY_ON_FREE       1
#define configSTATS_BUFFER_MAX_LENGTH         0xFFFF
#define configUSE_NEWLIB_REENTRANT            0

/* ===========================================================================
 * 软件定时器
 * =========================================================================== */
#define configUSE_TIMERS                      1
#define configTIMER_TASK_PRIORITY             ( configMAX_PRIORITIES - 1 )
#define configTIMER_TASK_STACK_DEPTH          ( configMINIMAL_STACK_SIZE * 2 )
#define configTIMER_QUEUE_LENGTH              10

/* ===========================================================================
 * 可选功能
 * =========================================================================== */
#define configUSE_EVENT_GROUPS                1
#define configUSE_STREAM_BUFFERS              1
#define configUSE_TASK_NOTIFICATIONS          1
#define configUSE_MUTEXES                     1
#define configUSE_RECURSIVE_MUTEXES           1
#define configUSE_COUNTING_SEMAPHORES         1
#define configUSE_QUEUE_SETS                  0
#define configUSE_APPLICATION_TASK_TAG        0

/* ===========================================================================
 * 内存管理（heap_4.c：带合并的非定长分配）
 * =========================================================================== */
#define configSUPPORT_STATIC_ALLOCATION       1
#define configSUPPORT_DYNAMIC_ALLOCATION      1
#define configTOTAL_HEAP_SIZE                 ( 32 * 1024 )   /* 32 KB，占 128 KB SRAM 的 1/4 */
#define configAPPLICATION_ALLOCATED_HEAP      0
#define configSTACK_ALLOCATION_FROM_SEPARATE_HEAP  0
#define configENABLE_HEAP_PROTECTOR           0
/* 由内核提供 vApplicationGetIdleTaskMemory / vApplicationGetTimerTaskMemory
 * （tasks.c 里由 configKERNEL_PROVIDED_STATIC_MEMORY==1 且非 MPU 移植层时启用），
 * 所以应用层不要再定义这两个函数。 */
#define configKERNEL_PROVIDED_STATIC_MEMORY   1

/* ===========================================================================
 * 中断嵌套行为（Cortex-M4，STM32F4 的 NVIC 实现 4 位优先级）
 * =========================================================================== */
#define configPRIO_BITS                       4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )
#define configMAX_API_CALL_INTERRUPT_PRIORITY  configMAX_SYSCALL_INTERRUPT_PRIORITY

/* ===========================================================================
 * 钩子函数开关
 * =========================================================================== */
#define configUSE_IDLE_HOOK                   0
#define configUSE_TICK_HOOK                   0
#define configUSE_MALLOC_FAILED_HOOK          1   /* 需要 vApplicationMallocFailedHook */
#define configUSE_DAEMON_TASK_STARTUP_HOOK    0
#define configUSE_SB_COMPLETED_CALLBACK       0
#define configCHECK_FOR_STACK_OVERFLOW        2   /* 需要 vApplicationStackOverflowHook */

/* ===========================================================================
 * 运行时统计 / trace
 * =========================================================================== */
#define configGENERATE_RUN_TIME_STATS         0
#define configUSE_TRACE_FACILITY              0
#define configUSE_STATS_FORMATTING_FUNCTIONS  0

/* ===========================================================================
 * 协程（不使用）
 * =========================================================================== */
#define configUSE_CO_ROUTINES                 0
#define configMAX_CO_ROUTINE_PRIORITIES       1

/* ===========================================================================
 * API 裁剪
 * =========================================================================== */
#define INCLUDE_vTaskPrioritySet              1
#define INCLUDE_uxTaskPriorityGet             1
#define INCLUDE_vTaskDelete                   1
#define INCLUDE_vTaskSuspend                  1
#define INCLUDE_vTaskDelayUntil               1
#define INCLUDE_vTaskDelay                    1
#define INCLUDE_xTaskGetSchedulerState        1
#define INCLUDE_xTaskGetCurrentTaskHandle     1
#define INCLUDE_uxTaskGetStackHighWaterMark   1
#define INCLUDE_xTaskGetIdleTaskHandle        1
#define INCLUDE_eTaskGetState                 1
#define INCLUDE_xResumeFromISR                1
#define INCLUDE_xTaskResumeFromISR            1
#define INCLUDE_xEventGroupSetBitFromISR      1
#define INCLUDE_xTimerPendFunctionCall        1
#define INCLUDE_xTaskAbortDelay               1
#define INCLUDE_xTaskGetHandle                1

/* ===========================================================================
 * 断言
 * ---------------------------------------------------------------------------
 * 失败时关中断、把文件名/行号记到全局变量（调试器可看），然后原地死循环。
 * 实现在 Core/Src/freertos_hooks.c。
 * =========================================================================== */
void vRtosAssertFailed( const char * pcFileName, unsigned long ulLineNumber );
#define configASSERT( x )                              \
    do {                                               \
        if( ( x ) == 0 )                               \
        {                                              \
            vRtosAssertFailed( __FILE__, __LINE__ );   \
        }                                              \
    } while( 0 )

/* ===========================================================================
 * 异常/中断向量归属（关键）
 * ---------------------------------------------------------------------------
 * FreeRTOS 移植层里的 vPortSVCHandler / xPortPendSVHandler / xPortSysTickHandler
 * 通过下面三行改名成 CMSIS 标准名，从而接管启动文件 startup_stm32f407xx.s 向量表
 * 中的 SVC_Handler / PendSV_Handler / SysTick_Handler。
 *
 * 这三个名字本来由 CubeMX 生成的 Core/Src/stm32f4xx_it.c 定义（空函数体），会造成
 * 重复符号 / RTOS 第一次上下文切换就死机。本项目**不修改** stm32f4xx_it.c（CubeMX
 * 每次重新生成都会覆盖它），而是在顶层 CMakeLists.txt 里只对该源文件加编译宏把它
 * 们的名字改掉：
 *     SVC_Handler=CubeMX_Unused_SVC_Handler
 *     PendSV_Handler=CubeMX_Unused_PendSV_Handler
 *     SysTick_Handler=CubeMX_Unused_SysTick_Handler
 * 这样每次 CubeMX 重新生成后都自动成立，无需人工修补。
 *
 * 前提：CubeMX 里 HAL 时基必须保持 TIM7（stm32f4xx_hal_timebase_tim.c），
 *       SysTick 才能完整让给 RTOS。
 * =========================================================================== */
#define vPortSVCHandler                       SVC_Handler
#define xPortPendSVHandler                    PendSV_Handler
#define xPortSysTickHandler                   SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
