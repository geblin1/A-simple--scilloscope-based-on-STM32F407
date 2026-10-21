/**
  * @file    ADC_NoiseTest.c
  * @brief   ADC 噪声 A/B 对比实现（见 ADC_NoiseTest.h 的说明）
  */

#include "ADC_NoiseTest.h"

#include <stdio.h>

#include "main.h"
#include "ADC_Drv.h"
#include "Frontend.h"
#include "ScopeView.h"

/* ---------------- 试验参数 ---------------- */
#define NOISE_SAMPLES    320U        /* 一帧点数，与示波器一致 */
#define NOISE_ROUNDS     6U          /* 每组跑几次，取平均与最差 */
#define NOISE_RATE_HZ    40000U      /* 与默认时基 1 ms/div 对应 */
#define NOISE_TIMEOUT_MS 200U
#define NOISE_PERIOD_MS  2000U       /* 两轮之间歇多久 */

typedef struct
{
    uint16_t lo1;
    uint16_t hi1;
    uint16_t lo2;
    uint16_t hi2;
} Noise_Range;

static uint32_t s_noise_raw[NOISE_SAMPLES];

/**
  * @brief  采一帧并统计两路的码值跨度
  * @param  out       输出：两路的 min/max
  * @param  with_draw 1 = 采集期间整屏重画（模拟 RTOS 版并行状态）
  * @retval 1 = 成功，0 = 采集失败
  */
static uint8_t Noise_Capture(Noise_Range *out, uint8_t with_draw)
{
    uint32_t i;
    uint32_t lo1 = 0xFFFFU;
    uint32_t hi1 = 0U;
    uint32_t lo2 = 0xFFFFU;
    uint32_t hi2 = 0U;

    if (ADC_Drv_CaptureStart(s_noise_raw, NOISE_SAMPLES) != ADC_DRV_OK)
    {
        return 0U;
    }

    if (with_draw != 0U)
    {
        /* 边采边刷屏：整屏 153 600 B 的 SPI 突发会在这 8 ms 的采样窗口里打满 */
        View_DrawFrame();
    }

    if (ADC_Drv_CaptureWait(NOISE_TIMEOUT_MS) != ADC_DRV_OK)
    {
        (void)ADC_Drv_CaptureStop();
        return 0U;
    }

    (void)ADC_Drv_CaptureStop();

    for (i = 0U; i < NOISE_SAMPLES; i++)
    {
        uint32_t a1 = (uint32_t)ADC_Drv_WordCh1(s_noise_raw[i]);
        uint32_t a2 = (uint32_t)ADC_Drv_WordCh2(s_noise_raw[i]);

        if (a1 < lo1) { lo1 = a1; }
        if (a1 > hi1) { hi1 = a1; }
        if (a2 < lo2) { lo2 = a2; }
        if (a2 > hi2) { hi2 = a2; }
    }

    out->lo1 = (uint16_t)lo1;
    out->hi1 = (uint16_t)hi1;
    out->lo2 = (uint16_t)lo2;
    out->hi2 = (uint16_t)hi2;

    return 1U;
}

/**
  * @brief  把"码值跨度"换算成 mV（按当前前端量程的 LSB 折算）
  */
static uint32_t Noise_CodesToMv(uint32_t codes)
{
    int32_t full = Frontend_RawToMv(FRONTEND_CH1, ADC_DRV_FULL_SCALE)
                   - Frontend_RawToMv(FRONTEND_CH1, 0U);

    if ((full <= 0) || (codes == 0U))
    {
        return 0U;
    }

    return (uint32_t)(((int32_t)codes * full) / (int32_t)ADC_DRV_FULL_SCALE);
}

/**
  * @brief  跑一组（A 或 B）并打印统计
  */
static void Noise_Group(const char *tag, uint8_t with_draw)
{
    Noise_Range r;
    uint32_t    round;
    uint32_t    good = 0U;
    uint32_t    sum1 = 0U;
    uint32_t    sum2 = 0U;
    uint32_t    max1 = 0U;
    uint32_t    max2 = 0U;

    for (round = 0U; round < NOISE_ROUNDS; round++)
    {
        uint32_t d1;
        uint32_t d2;

        if (Noise_Capture(&r, with_draw) == 0U)
        {
            continue;
        }

        d1 = (uint32_t)(r.hi1 - r.lo1);
        d2 = (uint32_t)(r.hi2 - r.lo2);

        sum1 += d1;
        sum2 += d2;

        if (d1 > max1) { max1 = d1; }
        if (d2 > max2) { max2 = d2; }

        good++;
    }

    if (good == 0U)
    {
        (void)printf("[noise] %s: capture FAILED\r\n", tag);
        return;
    }

    (void)printf("[noise] %s: CH1 spread avg %lu max %lu codes (%lu mV) | "
                 "CH2 spread avg %lu max %lu codes (%lu mV)\r\n",
                 tag,
                 (unsigned long)(sum1 / good), (unsigned long)max1,
                 (unsigned long)Noise_CodesToMv(max1),
                 (unsigned long)(sum2 / good), (unsigned long)max2,
                 (unsigned long)Noise_CodesToMv(max2));
}

void ADC_NoiseTest_Run(void)
{
    static uint8_t s_banner_done = 0U;
    uint32_t       actual = 0U;

    if (s_banner_done == 0U)
    {
        (void)ADC_Drv_SetSampleRate(NOISE_RATE_HZ, &actual);

        (void)printf("\r\n[noise] A/B test: %lu Sa/s, %u pts/frame, %u rounds each\r\n",
                     (unsigned long)actual,
                     (unsigned int)NOISE_SAMPLES,
                     (unsigned int)NOISE_ROUNDS);
        (void)printf("[noise] A = capture only        (bare-metal behaviour: sample, then draw)\r\n");
        (void)printf("[noise] B = capture + full redraw (RTOS behaviour: sampling while SPI runs)\r\n");

        s_banner_done = 1U;
    }

    Noise_Group("A  no-draw ", 0U);
    Noise_Group("B  drawing ", 1U);

    HAL_Delay(NOISE_PERIOD_MS);
}
