/**
  ******************************************************************************
  * @file    freertos_hooks.c
  * @brief   FreeRTOS 内核要求的应用层钩子函数（均为内核强符号依赖，不能省略）
  *
  * 本文件不属于 STM32CubeMX 管理范围，重新生成代码不会影响它。
  *
  * 依赖关系（见 Core/Inc/FreeRTOSConfig.h）：
  *   configCHECK_FOR_STACK_OVERFLOW = 2  -> 必须有 vApplicationStackOverflowHook()
  *   configUSE_MALLOC_FAILED_HOOK   = 1  -> 必须有 vApplicationMallocFailedHook()
  *   configKERNEL_PROVIDED_STATIC_MEMORY = 1 -> 内核自己提供
  *                                            vApplicationGetIdleTaskMemory() 与
  *                                            vApplicationGetTimerTaskMemory()，
  *                                            应用层**不要**再定义这两个函数。
  *
  * 出错处理策略：关中断 + 记录现场（全局变量，调试器可直接观察）+ 原地死循环。
  * 本工程当前没有配置任何 GPIO 引脚，所以不做 LED 闪烁提示。
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"

/* 出错现场（调试时在 Watch 窗口观察这几个变量） -----------------------------*/
volatile unsigned long g_ulRtosAssertLine    = 0;   /* configASSERT 失败行号 */
volatile const char  *g_pcRtosAssertFile     = 0;   /* configASSERT 失败文件名 */
volatile unsigned long g_ulStackOverflowFlag = 0;   /* 栈溢出次数 */
volatile const char  *g_pcStackOverflowTask  = 0;   /* 溢出任务名 */
volatile unsigned long g_ulMallocFailedCount = 0;   /* 堆耗尽次数 */

/**
  * @brief  configASSERT() 失败时调用（见 FreeRTOSConfig.h）
  * @param  pcFileName   源文件名（__FILE__）
  * @param  ulLineNumber 行号（__LINE__）
  */
void vRtosAssertFailed( const char * pcFileName, unsigned long ulLineNumber )
{
    taskDISABLE_INTERRUPTS();

    g_pcRtosAssertFile = pcFileName;
    g_ulRtosAssertLine = ulLineNumber;

    for( ; ; )
    {
        /* 断点打在这里，然后看 g_pcRtosAssertFile / g_ulRtosAssertLine */
    }
}

/**
  * @brief  任务栈溢出钩子（configCHECK_FOR_STACK_OVERFLOW = 2 时由内核调用）
  * @param  xTask       发生溢出的任务句柄
  * @param  pcTaskName  任务名
  */
void vApplicationStackOverflowHook( TaskHandle_t xTask, char * pcTaskName )
{
    ( void ) xTask;

    taskDISABLE_INTERRUPTS();

    g_pcStackOverflowTask = pcTaskName;
    g_ulStackOverflowFlag++;

    for( ; ; )
    {
        /* 断点打在这里，然后看 g_pcStackOverflowTask */
    }
}

/**
  * @brief  堆分配失败钩子（configUSE_MALLOC_FAILED_HOOK = 1 时由内核调用）
  *         常见原因：configTOTAL_HEAP_SIZE 太小，或任务栈/队列申请得太多。
  */
void vApplicationMallocFailedHook( void )
{
    taskDISABLE_INTERRUPTS();

    g_ulMallocFailedCount++;

    for( ; ; )
    {
        /* 断点打在这里，然后调大 FreeRTOSConfig.h 里的 configTOTAL_HEAP_SIZE */
    }
}
