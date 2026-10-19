/**
  * @file    ADC_DMA_Test.c
  * @brief   双 ADC 同步 + 定时器触发 + DMA 帧采集示例 —— 只调用 ADC_Drv 接口
  *
  * 用途：验证“按设定采样率采满一帧”这条链路，并反算实际采样率。
  *       所有底层动作（TIM2 触发、DMA 启动、CR2.DMA 置位、结束回收）
  *       都在 ADC_Drv.c 里，这里只做请求、统计和打印，
  *       可以作为“业务层怎么用帧采集 API”的范例。
  */

#include "ADC_DMA_Test.h"

#include "main.h"
#include "ADC_Drv.h"

#include <stdio.h>

#define ADC_DMA_TEST_N           1024U
#define ADC_DMA_TEST_EMPTY       0xFFFFFFFFU   /* 未被 DMA 覆盖的哨兵值（12 位数据不可能是全 F） */
#define ADC_DMA_TEST_TIMEOUT_MS  100U
#define ADC_DMA_TEST_CORE_HZ     168000000U    /* SYSCLK，用于把 DWT 周期数换算成时间 */

static uint32_t s_adc_dma_buf[ADC_DMA_TEST_N];
static uint32_t s_cycles;

/** @brief 打开 DWT 周期计数器，用于测量帧采集耗时 */
static void ADC_DMA_Test_DwtInit(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/**
  * @brief  采一帧
  * @param  buf 目标缓冲区（SRAM）
  * @param  len 点数
  * @retval 1 = 采满，0 = 失败/超时
  */
uint8_t ADC_DMA_Test_Once(uint32_t *buf, uint32_t len)
{
    uint32_t i;
    uint32_t c0;
    uint32_t limit_cycles;

    if ((buf == NULL) || (len == 0U) || (len > ADC_DRV_CAPTURE_MAX))
    {
        return 0U;
    }

    for (i = 0U; i < len; i++)
    {
        buf[i] = ADC_DMA_TEST_EMPTY;
    }

    ADC_DMA_Test_DwtInit();
    limit_cycles = (uint32_t)(((uint64_t)ADC_DMA_TEST_CORE_HZ * (uint64_t)ADC_DMA_TEST_TIMEOUT_MS) / 1000ULL);

    c0 = DWT->CYCCNT;
    if (ADC_Drv_CaptureStart(buf, len) != ADC_DRV_OK)
    {
        return 0U;
    }

    /* 这里要测时间，所以用紧凑轮询而不是 ADC_Drv_CaptureWait()；
       业务代码在 RTOS 任务里请直接用 ADC_Drv_CaptureWait() 阻塞等待。 */
    while (ADC_Drv_CaptureIsDone() == 0U)
    {
        if ((DWT->CYCCNT - c0) > limit_cycles)
        {
            (void)ADC_Drv_CaptureStop();
            return 0U;
        }
    }

    s_cycles = DWT->CYCCNT - c0;
    (void)ADC_Drv_CaptureStop();
    return 1U;
}

/**
  * @brief  统计并打印上一帧
  */
static void ADC_DMA_Test_Report(const char *tag)
{
    uint32_t i;
    uint32_t filled = 0U;
    uint32_t sum1 = 0U;
    uint32_t sum2 = 0U;
    uint32_t lo1 = 0xFFFFFFFFU;
    uint32_t hi1 = 0U;
    uint32_t lo2 = 0xFFFFFFFFU;
    uint32_t hi2 = 0U;
    uint32_t maxdiff = 0U;
    uint32_t rate = ADC_Drv_GetSampleRate();
    uint32_t meas = 0U;

    for (i = 0U; i < ADC_DMA_TEST_N; i++)
    {
        uint32_t word = s_adc_dma_buf[i];
        uint32_t v1;
        uint32_t v2;
        uint32_t d;

        if (word == ADC_DMA_TEST_EMPTY)
        {
            continue;
        }

        v1 = (uint32_t)ADC_Drv_WordCh1(word);
        v2 = (uint32_t)ADC_Drv_WordCh2(word);

        filled++;
        sum1 += v1;
        sum2 += v2;

        if (v1 < lo1) { lo1 = v1; }
        if (v1 > hi1) { hi1 = v1; }
        if (v2 < lo2) { lo2 = v2; }
        if (v2 > hi2) { hi2 = v2; }

        d = (v1 > v2) ? (v1 - v2) : (v2 - v1);
        if (d > maxdiff) { maxdiff = d; }
    }

    if (filled == 0U)
    {
        printf("%s: 一个点都没搬进来\r\n", tag);
        return;
    }

    if (s_cycles != 0U)
    {
        meas = (uint32_t)(((uint64_t)filled * (uint64_t)ADC_DMA_TEST_CORE_HZ) / (uint64_t)s_cycles);
    }

    printf("%s: 设定 %lu SPS | 搬满 %lu/%lu 点 | 实测 %lu SPS | 周期数 %lu\r\n",
           tag,
           (unsigned long)rate,
           (unsigned long)filled,
           (unsigned long)ADC_DMA_TEST_N,
           (unsigned long)meas,
           (unsigned long)s_cycles);

    printf("   w0=%08lX w1=%08lX | ADC1 avg=%lu(%lumV) rng %lu-%lu | "
           "ADC2 avg=%lu(%lumV) rng %lu-%lu | maxdiff %lu\r\n",
           (unsigned long)s_adc_dma_buf[0],
           (unsigned long)s_adc_dma_buf[1],
           (unsigned long)(sum1 / filled), (unsigned long)ADC_Drv_ToMillivolt((uint16_t)(sum1 / filled)),
           (unsigned long)lo1, (unsigned long)hi1,
           (unsigned long)(sum2 / filled), (unsigned long)ADC_Drv_ToMillivolt((uint16_t)(sum2 / filled)),
           (unsigned long)lo2, (unsigned long)hi2,
           (unsigned long)maxdiff);
}

/**
  * @brief  采集测试主流程：200kSPS 与 500kSPS 两档，并演示阻塞等待接口
  */
void ADC_DMA_Test_Run(void)
{
    static uint8_t banner_done = 0U;

    if (banner_done == 0U)
    {
        banner_done = 1U;
        printf("\r\nADC+DMA+TIM2 test: N=%lu, 触发=TIM2 TRGO(UPDATE), DMA2_Stream0 普通模式\r\n",
               (unsigned long)ADC_DMA_TEST_N);
    }

    (void)ADC_Drv_SetSampleRate(200000U, NULL);
    if (ADC_DMA_Test_Once(s_adc_dma_buf, ADC_DMA_TEST_N) != 0U)
    {
        ADC_DMA_Test_Report("200k");
    }
    else
    {
        printf("200k: 超时，DMA 没搬满（NDTR 未归零）——检查 TIM2 是否真的在触发\r\n");
    }

    (void)ADC_Drv_SetSampleRate(500000U, NULL);
    if (ADC_DMA_Test_Once(s_adc_dma_buf, ADC_DMA_TEST_N) != 0U)
    {
        ADC_DMA_Test_Report("500k");
    }
    else
    {
        printf("500k: 超时，DMA 没搬满（NDTR 未归零）——检查 TIM2 是否真的在触发\r\n");
    }

    /* 演示任务里应该用的阻塞接口：起一帧然后等它完成 */
    if (ADC_Drv_CaptureStart(s_adc_dma_buf, ADC_DMA_TEST_N) == ADC_DRV_OK)
    {
        ADC_Drv_Status st = ADC_Drv_CaptureWait(ADC_DMA_TEST_TIMEOUT_MS);
        printf("CaptureWait(): %s\r\n", (st == ADC_DRV_OK) ? "OK" : (st == ADC_DRV_TIMEOUT) ? "TIMEOUT" : "ERROR");
    }
}
