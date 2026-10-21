/*
 * @Author: geblin1 3390931275@qq.com
 * @Date: 2026-10-12 20:10:44
 * @LastEditors: geblin1 3390931275@qq.com
 * @LastEditTime: 2026-10-20 22:20:00
 * @FilePath: \基于STM32F407ZGT6的简易示波器\Core\Src\main.c
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dma.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* FreeRTOS 应用层入口（内核本身由 ThirdParty/FreeRTOS 与 cmake 集成，见顶层 CMakeLists.txt） */
#include "FreeRTOS.h"
#include "stm32f407xx.h"
#include "stm32f4xx_hal.h"
#include "task.h"
#include "freertos_app.h"
#include "SPI.h"
#include "Key.h"
#include "LED.h"
#include <stdio.h>          /* printf: 已由 User/BSP/Src/Retarget.c 重定向到 USART1 */
#include "ADC_Drv.h"        /* 双 ADC 采集驱动：业务层只调这里的接口 */
#include "Frontend.h"       /* 前端标定表：上电从 flash 读回逐通道逐量程参数 */
#include "ADC_Test.h"       /* ADC1/ADC2 自检（User/BSP/Src/ADC_Test.c） */
#include "ADC_DMA_Test.h"   /* 双 ADC 同步 + DMA 采集测试（User/BSP/Src/ADC_DMA_Test.c） */
#include "Encoder.h"
#include "App_Scope.h"      /* 示波器应用层：输入、采集、界面 */
#include "App_Rtos.h"       /* 任务层：采集 / 界面 / 监控三个任务 */
#include "ADC_SelfCheck.h"  /* 前端偏置改造自检（纯串口输出） */
#include "ADC_NoiseTest.h"  /* 噪声 A/B 对比：判断刷屏耦合还是前端本身（串口输出） */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* 前端改造自检开关：
   1 = 只跑串口自检（跳过 LCD 初始化），用于核对偏置电路
   0 = 正常跑示波器界面
   自检做完后把这里改回 0 重新编译即可，其他代码不用动。 */
#define APP_RUN_SELFCHECK   0

/* 噪声 A/B 对比开关：
   1 = 只跑 A/B 对比并串口打印（不启动 RTOS，但需要 LCD 初始化正常），
   0 = 正常跑示波器界面。
   用途：判断"波形噪声大"是 LCD 刷屏耦合进采样窗口，还是前端电路本身，
   拿到数字后改回 0 即可。 */
#define APP_RUN_NOISETEST   0
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_TIM8_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_TIM4_Init();
  MX_TIM2_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  /* ADC 驱动初始化：必须在 MX_ADC1_Init()/MX_ADC2_Init() 之后、开始采集之前 */
  (void)ADC_Drv_Init();

  /* 前端标定表初始化：优先从 flash 扇区 11 读回上次的两点标定结果，
     读不到（首上电 / 校验失败）就用设计默认值。必须在任何采集之前。 */
  Frontend_Init();

#if (APP_RUN_SELFCHECK != 0)
  /* 前端自检模式：不初始化 LCD、不启动调度器，直接走下面的裸机循环 */
#elif (APP_RUN_NOISETEST != 0)
  /* 噪声 A/B 模式：需要 LCD 与视图层就绪，但不启动调度器 */
  App_Scope_Init();
#else
  /* 界面初始化：LCD 上电、画框架、装载默认参数
     必须在 MX_SPI1_Init() 与 ADC_Drv_Init() 之后、调度器启动之前 */
  App_Scope_Init();

  /* 创建采集 / 界面 / 监控三个任务并启动调度器；正常情况下不返回。
     自检模式（APP_RUN_SELFCHECK == 1）不启动 RTOS，走下面的裸机循环。 */
  App_Rtos_Start();
#endif

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
#if (APP_RUN_SELFCHECK != 0)
    /* 前端偏置自检（裸机模式）：每 500 ms 往 USART1 打一行读数 */
    ADC_SelfCheck_Run();
#elif (APP_RUN_NOISETEST != 0)
    /* 噪声 A/B 对比（裸机模式）：先不刷屏采一组、再边刷屏边采一组 */
    ADC_NoiseTest_Run();
#else
    /* 正常模式走不到这里：App_Rtos_Start() 启动调度器后不返回。
       采集与界面由 acq / ui 两个任务分别推进，见 User/App/Src/App_Rtos.c。 */
#endif
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM7 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM7)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */
  if (htim->Instance == TIM7)
  {
    Key_Tick();
    /* LED 心跳：每 500 ms 翻转一次，RUN / HOLD 都一样。
       放在 1 ms 中断里而不是任务里——界面任务刷一帧要几十毫秒，
       低优先级任务会被饿死，放任务里闪烁就不准了。 */
    LED_Tick();
  }
  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
