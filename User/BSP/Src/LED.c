/*
 * @Author: geblin1 3390931275@qq.com
 * @Date: 2026-10-19 18:37:25
 * @LastEditors: geblin1 3390931275@qq.com
 * @LastEditTime: 2026-10-19 18:54:08
 * @FilePath: \基于STM32F407ZGT6的简易示波器\User\BSP\Src\LED.c
 * @Description: LED驱动程序，用于显示示波器运行状态
 */
#include "LED.h"
#include "gpio.h"
#include "stm32f407xx.h"
#include "stm32f4xx_hal_gpio.h"
void LED_ON()
{
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, GPIO_PIN_SET);
}
void LED_OFF()
{
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, GPIO_PIN_RESET);
}
void LED_Turn()
{
    if(HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_8) == 0)
    {
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, GPIO_PIN_SET);
    }
    else if(HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_8) == 1)
    {
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, GPIO_PIN_RESET);
    }
}

/* 心跳计数：由 1 ms 的 TIM7 中断累加（见 Core/Src/main.c）。
 * 之所以放在中断里而不是放在任务里：界面任务刷一帧就要几十毫秒，
 * 低优先级的监控任务会被饿死，闪烁就会变得忽快忽慢；
 * 中断由硬件定时器驱动，不受任务调度影响，状态显示才可信。 */
static uint32_t s_led_ms = 0U;

void LED_Tick(void)
{
    /* 每 500 ms 翻转一次：节奏只由 1 ms 硬件定时中断决定，
     * 与任务调度、画面负载无关，所以 RUN / HOLD 都一样地稳定闪。 */
    s_led_ms++;
    if (s_led_ms >= 500U)
    {
        s_led_ms = 0U;
        LED_Turn();
    }
}