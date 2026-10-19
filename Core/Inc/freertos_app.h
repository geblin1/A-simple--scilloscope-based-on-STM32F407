/**
  ******************************************************************************
  * @file    freertos_app.h
  * @brief   FreeRTOS 应用层入口（对应参考工程 Core/Src/freertos_demo.c 里的
  *          freertos_start()，本工程命名为 freertos_app_start()）
  *
  * 用法：在 Core/Src/main.c 的 "USER CODE BEGIN 2" 区段里、MX_xxx_Init()
  *       全部调用完之后调用 freertos_app_start()；它会创建任务并启动调度器，
  *       正常情况下一去不复返（main() 里 while(1) 之后的代码永远不会执行）。
  ******************************************************************************
  */

#ifndef FREERTOS_APP_H
#define FREERTOS_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 运行时统计量：不接线也能验证 RTOS 是否在跑——用调试器把它们加进 Watch 窗口。
 * 调度正常时：g_ulRtosSecondsCount 每秒 +1，g_ulRtosProducedCount 与
 * g_ulRtosConsumedCount 每 500 ms 各 +1，g_ulRtosTimerCount 每秒 +1。 */
extern volatile uint32_t g_ulRtosProducedCount;   /* 生产者任务已发送的采样条数 */
extern volatile uint32_t g_ulRtosConsumedCount;   /* 消费者任务已取走的采样条数 */
extern volatile uint32_t g_ulRtosSecondsCount;    /* 运行秒数 */
extern volatile uint32_t g_ulRtosTimerCount;      /* 软件定时器回调次数 */
extern volatile uint32_t g_ulRtosLastSample;      /* 最近一条采样的值 */
extern volatile uint32_t g_ulRtosHeapFreeBytes;   /* 剩余堆空间（字节） */

/**
  * @brief  创建 FreeRTOS 任务/队列/定时器并启动调度器。
  * @note   本函数不返回（除非堆不足导致调度器启动失败）。
  */
void freertos_app_start( void );

#ifdef __cplusplus
}
#endif

#endif /* FREERTOS_APP_H */
