/**
  * @file    ADC_Test.c
  * @brief   ADC1/ADC2 自检示例 —— 只调用 ADC_Drv 接口，不碰任何底层寄存器
  *
  * 用途：确认两路 ADC 能正常读到电压。全部逻辑在 ADC_Drv.c 里，
  *       这里只做统计和打印，可以作为“业务层怎么用 ADC API”的范例。
  */

#include "ADC_Test.h"
#include "ADC_Drv.h"

#include <stdio.h>

#define ADC_TEST_SAMPLES   8U

/**
  * @brief  判定一路采样结果是否正常
  */
static const char *ADC_Test_Verdict(uint32_t lo, uint32_t hi)
{
    if (hi == 0U)
    {
        return "STUCK-0";                 /* 一直是 0：悬空或没接信号 */
    }
    if ((lo == hi) && (lo >= 4090U))
    {
        return "STUCK-MAX";               /* 一直顶到满量程：可能接的就是 3.3V */
    }
    return "OK";
}

/**
  * @brief  读一次两路原始值（0~4095）
  * @retval 1 = 成功，0 = 失败
  */
uint8_t ADC_Test_Sample(uint32_t *adc1, uint32_t *adc2)
{
    uint16_t ch1 = 0U;
    uint16_t ch2 = 0U;

    if ((adc1 == NULL) || (adc2 == NULL))
    {
        return 0U;
    }
    if (ADC_Drv_ReadOnce(&ch1, &ch2) != ADC_DRV_OK)
    {
        return 0U;
    }

    *adc1 = (uint32_t)ch1;
    *adc2 = (uint32_t)ch2;
    return 1U;
}

/**
  * @brief  自检主流程：采 8 次，打印平均值、范围、判定
  */
void ADC_Test_Run(void)
{
    static uint8_t banner_done = 0U;

    uint32_t lo1 = 0xFFFFFFFFU;
    uint32_t hi1 = 0U;
    uint32_t lo2 = 0xFFFFFFFFU;
    uint32_t hi2 = 0U;
    uint32_t sum1 = 0U;
    uint32_t sum2 = 0U;
    uint16_t ch1 = 0U;
    uint16_t ch2 = 0U;
    uint32_t i;

    if (banner_done == 0U)
    {
        banner_done = 1U;
        printf("\r\nADC self-test: ADC1 = PA0(IN0), ADC2 = PA1(IN1), 12bit, VREF %lumV\r\n",
               (unsigned long)ADC_DRV_VREF_MV);
    }

    for (i = 0U; i < ADC_TEST_SAMPLES; i++)
    {
        if (ADC_Drv_ReadOnce(&ch1, &ch2) != ADC_DRV_OK)
        {
            printf("FAIL: ADC_Drv_ReadOnce() error\r\n");
            printf("check that ADC_Drv_Init() ran after MX_ADC1_Init()/MX_ADC2_Init()\r\n");
            return;
        }

        if ((uint32_t)ch1 < lo1) { lo1 = (uint32_t)ch1; }
        if ((uint32_t)ch1 > hi1) { hi1 = (uint32_t)ch1; }
        if ((uint32_t)ch2 < lo2) { lo2 = (uint32_t)ch2; }
        if ((uint32_t)ch2 > hi2) { hi2 = (uint32_t)ch2; }

        sum1 += (uint32_t)ch1;
        sum2 += (uint32_t)ch2;
    }

    {
        uint32_t avg1 = sum1 / ADC_TEST_SAMPLES;
        uint32_t avg2 = sum2 / ADC_TEST_SAMPLES;
        uint32_t mv1  = (uint32_t)ADC_Drv_ToMillivolt((uint16_t)avg1);
        uint32_t mv2  = (uint32_t)ADC_Drv_ToMillivolt((uint16_t)avg2);

        printf("ADC1(PA0)=%4lu(%4lumV) rng %4lu-%4lu [%s] | "
               "ADC2(PA1)=%4lu(%4lumV) rng %4lu-%4lu [%s] | diff %ldmV\r\n",
               (unsigned long)avg1, (unsigned long)mv1,
               (unsigned long)lo1, (unsigned long)hi1, ADC_Test_Verdict(lo1, hi1),
               (unsigned long)avg2, (unsigned long)mv2,
               (unsigned long)lo2, (unsigned long)hi2, ADC_Test_Verdict(lo2, hi2),
               (long)mv1 - (long)mv2);
    }
}
