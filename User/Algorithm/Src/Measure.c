/**
  * @file    Measure.c
  * @brief   波形测量算法实现（输入为已标定的 mV，带符号）
  *
  * 频率是怎么测的
  * --------------
  * 以均值为中心设一个带滞回的门限（h = 幅度/8，最小 5 mV）：
  * 信号先低于"低门限"再高于"高门限"算一次上升沿。
  * 记录第一次和最后一次上升沿的样本下标，用 (次数-1) 个周期求平均。
  * 滞回可以避免噪声在门限附近反复触发。
  *
  * 为什么不用浮点
  * --------------
  * 本工程没开 printf 浮点，也不希望引入 libm；整数定点足够显示到 3 位有效数字。
  *
  * 单位约定
  * --------
  * 输入数组是"探头看到的电压"，单位 mV，可以为负。
  * 因为已经在 Frontend 里扣掉了偏置零点，这里的均值就是真实的直流分量。
  */

#include "Measure.h"

#define MEASURE_MAX_EDGES       16U
#define MEASURE_MIN_AMP_MV      60L     /* 峰峰值小于这个就当没有交流信号 */
#define MEASURE_HYST_MIN_MV     5L      /* 滞回窗口的下限 */

/* 整数平方根（牛顿迭代） */
static uint32_t Measure_Sqrt(uint32_t v)
{
    uint32_t x;
    uint32_t y;

    if (v == 0U)
    {
        return 0U;
    }

    x = v;
    y = (x + 1U) / 2U;
    while (y < x)
    {
        x = y;
        y = (x + (v / x)) / 2U;
    }
    return x;
}

void Measure_Analyze(const int32_t *samples, uint16_t count,
                     uint32_t sample_rate, Measure_Result *out)
{
    uint16_t i;
    int64_t  sum = 0;
    uint64_t sum_sq = 0U;
    int32_t  vmin = 0;
    int32_t  vmax = 0;
    int32_t  mean_mv;

    if (out == 0)
    {
        return;
    }

    out->valid     = 0U;
    out->vpp_mv    = 0;
    out->rms_mv    = 0;
    out->mean_mv   = 0;
    out->freq_mhz  = 0;
    out->period_us = 0;

    if ((samples == 0) || (count == 0U))
    {
        return;
    }

    /* ---- 基本统计 ---- */
    vmin = samples[0];
    vmax = samples[0];

    for (i = 0U; i < count; i++)
    {
        int32_t s = samples[i];

        sum += (int64_t)s;
        sum_sq += (uint64_t)((int64_t)s * (int64_t)s);

        if (s < vmin)
        {
            vmin = s;
        }
        if (s > vmax)
        {
            vmax = s;
        }
    }

    mean_mv = (int32_t)(sum / (int64_t)count);

    out->mean_mv = mean_mv;
    out->vpp_mv  = vmax - vmin;
    out->rms_mv  = (int32_t)Measure_Sqrt((uint32_t)(sum_sq / (uint64_t)count));
    out->valid   = 1U;

    /* ---- 频率 / 周期 ---- */
    if ((sample_rate == 0U) || ((int32_t)(vmax - vmin) < MEASURE_MIN_AMP_MV) || (count < 8U))
    {
        return;
    }

    {
        int32_t amp   = vmax - vmin;
        int32_t hyst  = amp / 8;
        int32_t lo_th;
        int32_t hi_th;
        uint16_t edges = 0U;
        uint8_t  armed = 0U;
        uint32_t first = 0U;
        uint32_t last  = 0U;

        if (hyst < MEASURE_HYST_MIN_MV)
        {
            hyst = MEASURE_HYST_MIN_MV;
        }

        lo_th = mean_mv - hyst;
        hi_th = mean_mv + hyst;

        for (i = 0U; i < count; i++)
        {
            int32_t s = samples[i];

            if (armed == 0U)
            {
                if (s < lo_th)
                {
                    armed = 1U;
                }
            }
            else if (s > hi_th)
            {
                armed = 0U;
                if (edges == 0U)
                {
                    first = i;
                }
                last = i;
                edges++;

                if (edges >= MEASURE_MAX_EDGES)
                {
                    break;
                }
            }
        }

        if (edges >= 2U)
        {
            uint32_t periods = (uint32_t)(edges - 1U);
            uint32_t spans   = last - first;

            if (spans > 0U)
            {
                /* 频率 = 采样率 * 周期数 / 样本跨度，单位 0.001 Hz */
                uint64_t f_hz = ((uint64_t)sample_rate * periods) / spans;

                out->freq_mhz = (int32_t)(f_hz * 1000U);

                /* 周期 = 跨度 * 1e6 / (采样率 * 周期数)，单位 us */
                out->period_us = (int32_t)(((uint64_t)spans * 1000000ULL) /
                                           ((uint64_t)sample_rate * periods));
            }
        }
    }
}
