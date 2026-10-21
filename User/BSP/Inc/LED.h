#ifndef __LED_H
#define __LED_H
#include <stdint.h>

void LED_ON();
void LED_OFF();
void LED_Turn();

/**
  * @brief  1 ms 心跳：每 500 ms 翻转一次 PC8（运行指示灯）
  * @note   必须由 1 ms 定时中断调用（TIM7，见 Core/Src/main.c 的
  *         HAL_TIM_PeriodElapsedCallback）。函数内只有寄存器写和一个静态计数，
  *         无阻塞、无 HAL 延时，所以节奏只由硬件定时器决定，
  *         即使某个任务被饿死，LED 也照常闪。
  */
void LED_Tick(void);

#endif
