/**
  * @file    App_Rtos.h
  * @brief   FreeRTOS 任务层入口：把示波器拆成采集 / 界面 / 监控三个任务
  *
  * 任务划分
  * --------
  *   任务      优先级     栈        节拍        职责
  *   acq       4 (较高)   1.0 KB    20 ms 起    采一帧 -> 换算 -> 测量 -> 发布
  *   ui        3          2.5 KB    10 ms       读输入 -> 改状态 -> 取新帧 -> 绘制
  *   monitor   1 (最低)   0.5 KB    500 ms      LED 心跳 + 堆/栈余量统计
  *
  * 为什么这样排
  * ------------
  *   1. 采集任务必须比界面任务高：LCD 通过 SPI 阻塞发送，一次整屏重画要几十毫秒，
  *      如果界面优先，采集就会被绘图拖慢、采样率变成"看绘图速度"。现在采集
  *      等 DMA 时阻塞在信号量上（让出 CPU），DMA 一完成就把界面任务抢占掉，
  *      先发布新帧再回去绘制，采集节拍与绘制耗时彻底解耦。
   *      注意 xSemaphoreGive() 只唤醒等待者、不转移所有权：若采完一帧不让出
   *      CPU，acq 会在下一轮开头立刻重新取走总线锁，界面任务永远抢不到（按键/
   *      编码器十几秒才响应一次）。所以每帧采完显式 vTaskDelay(1) 让出 1 tick。
  *   2. 监控任务永远最低：它只做统计，绝不能影响前两者。
  *   3. 三个任务都在等事件（信号量/延时）时不空转，空闲任务接管 CPU。
  *
  * 调用方式（main.c）
  * -----------------
  *   \code
  *   App_Scope_Init();     // 必须在调度器启动前
  *   App_Rtos_Start();     // 创建任务并启动调度器，正常情况下不返回
  *   \endcode
  */

#ifndef __APP_RTOS_H
#define __APP_RTOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  创建三个任务并启动 FreeRTOS 调度器
  * @note   正常情况下不返回（调度器接管 CPU）；
  *         只有任务创建失败（堆不够）时才会返回，函数内会挂起等待调试。
  */
void App_Rtos_Start(void);

/* 运行时健康指标（调试器 Watch 窗口直接看，或串口打印） */
extern volatile uint32_t g_monHeartbeat;      /* 监控任务计数，每 500 ms +1 */
extern volatile uint32_t g_monHeapFree;       /* 剩余堆字节数 */
extern volatile uint32_t g_monAcqStackMin;    /* 采集任务栈最小余量（字） */
extern volatile uint32_t g_monUiStackMin;     /* 界面任务栈最小余量（字） */
extern volatile uint32_t g_monUiFrames;       /* 界面任务重绘次数 */

#ifdef __cplusplus
}
#endif

#endif /* __APP_RTOS_H */
