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