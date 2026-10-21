/**
  * @file    App_Scope.h
  * @brief   示波器应用层入口（FreeRTOS 版本）
  *
  * 调用方式（main.c）
  * -----------------
  *   \code
  *   App_Scope_Init();          // 在 MX_SPI1_Init / ADC_Drv_Init 之后、调度器启动之前
  *   App_Rtos_Start();          // 创建采集/界面/监控任务并启动调度器（不返回）见 App_Rtos.h
  *   \endcode
  *
  * 本层只暴露两个"步进函数"，谁在什么节拍调用由任务层（App_Rtos.c）决定，
  * 这样应用逻辑与调度策略解耦：将来改成裸机只需要在 while(1) 里轮流调用它们。
  *
  * App 层负责：采集调度、按键与编码器解释、状态机、把结果显示到 View。
  * 它不直接碰 SPI、寄存器或 Font 位图。
  */

#ifndef __APP_SCOPE_H
#define __APP_SCOPE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  初始化 LCD、视图框架与默认参数，并创建帧交接用的互斥量
  * @note   必须在 MX_SPI1_Init() 与 ADC_Drv_Init() 之后调用；
  *         必须在 vTaskStartScheduler() 之前调用（互斥量在启动前创建是合法的）。
  */
void App_Scope_Init(void);

/**
  * @brief  采集一步：采一帧（640 点）-> 换算成 mV -> 触发查找 + 亚采样插值对齐
  *         -> 取 320 点显示窗 -> 测量 -> 发布给界面
  * @return 1 = 本步发布了新的一帧；0 = 当前处于 HOLD（未采集）
  * @note   只能由"采集任务"调用；内部会阻塞等待 DMA 完成（会让出 CPU），
  *         阻塞时长由当前采样率决定，最长约一帧时间（1 kSa/s 时约 640 ms）。
  */
uint8_t App_Scope_AcqStep(void);

/**
  * @brief  界面一步：读按键/编码器 -> 改状态 -> 取最新帧 -> 按需重绘
  * @return 1 = 本步有绘制动作；0 = 无事可做
  * @note   只能由"界面任务"调用。绘制是阻塞的 SPI 发送（一帧几十毫秒），
  *         因此界面任务优先级要低于采集任务，保证采集不被绘制拖慢。
  */
uint8_t App_Scope_UiStep(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_SCOPE_H */
