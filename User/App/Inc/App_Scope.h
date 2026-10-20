/**
  * @file    App_Scope.h
  * @brief   示波器应用层入口
  *
  * 调用方式（main.c）
  * -----------------
  *   \code
  *   App_Scope_Init();          // 在 MX_SPI1_Init / ADC_Drv_Init 之后
  *   for (;;) { App_Scope_Loop(); }
  *   \endcode
  *
  * App 层负责：采集调度、按键与编码器解释、状态机、把结果显示到 View。
  * 它不直接碰 SPI、寄存器或 Font 位图。
  */

#ifndef __APP_SCOPE_H
#define __APP_SCOPE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  初始化 LCD、视图框架与默认参数
  * @note   必须在 MX_SPI1_Init() 与 ADC_Drv_Init() 之后调用
  */
void App_Scope_Init(void);

/**
  * @brief  主循环体：处理输入、采集一帧、按需重绘
  * @note   无阻塞等待，可放在 while(1) 里反复调用
  */
void App_Scope_Loop(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_SCOPE_H */
