/**
  ******************************************************************************
  * @file    ADC_SelfCheck.h
  * @brief   模拟前端偏置改造自检（纯串口输出，不依赖 LCD）
  ******************************************************************************
  * 用途：电路改完之后，确认偏置网络是否接对。
  *       在 while(1) 里反复调用，每 500 ms 打印一次 CH1/CH2 的原始码值，
  *       并给出"按大量程解释 / 按小量程解释"两种输入电压，供对照
  *       FRONTEND_BIAS_DESIGN.md 第八节的自检表。
  *
  * 前提：printf 已重定向到 USART1（User/BSP/Src/Retarget.c），
  *       且 ADC_Drv_Init() 已调用。
  ******************************************************************************
  */
#ifndef __ADC_SELFCHECK_H
#define __ADC_SELFCHECK_H

#include <stdint.h>

/**
  * @brief  自检主循环，需在 while(1) 中反复调用（内部自带 500 ms 节流）
  * @retval None
  */
void ADC_SelfCheck_Run(void);

#endif /* __ADC_SELFCHECK_H */
