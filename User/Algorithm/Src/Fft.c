/**
  * @file    Fft.c
  * @brief   256 点定点 FFT（radix-2 DIT）+ Hann 窗 + 整数 dB 换算
  *
  * 为什么不用浮点
  * --------------
  *   本工程的测量、触发、标定全是整数定点（Measure.c / Trigger.c），
  *   不需要链接 libm / 软浮点，代码体积与时序都可预测。FFT 沿用同一思路：
  *     - 蝶形乘法用 Q15 旋转因子，int64 中间量防溢出；
  *     - 幅度换算成 10*log10(P/Pmax)，log2 用"指数 + Q8 尾数查表"实现。
  *
  * 量级检查
  * --------
  *   输入 |mV| <= 36300（大量程满幅），Hann 窗后 |x| <= 36300。
  *   最坏情况（全同号满幅）|X[0]| <= 36300 * 256/2 ≈ 4.6e6，int32 安全。
  *   功率 P = re^2 + im^2 <= 2*(4.6e6)^2 ≈ 4.3e13，用 int64。
  *   蝶形乘积 |b| * 32767 <= 4.6e6 * 32767 ≈ 1.5e11，用 int64。
  *
 * 去直流
 * ------
 *   窗函数会把直流泄漏到 bin1/bin2，纯直流信号会误显示成一条低谱线。
 *   所以在加窗前先减去 256 点的均值（残留直流由 bin0 隐藏）。
 */

#include "Fft.h"
#include "FftTables.h"

/* 64 位前导零个数（0..63）。v 必须 >= 1。 */
static uint8_t Fft_Clz64(uint64_t v)
{
    uint32_t hi = (uint32_t)(v >> 32);

    if (hi != 0U)
    {
        return (uint8_t)__builtin_clz(hi);
    }
    return (uint8_t)(32U + (uint32_t)__builtin_clz((uint32_t)v));
}

/* log2(v) 的 Q8 值（v >= 1）。
   v = 2^e * f（f in [1,2)），log2(v) = e + log2(f)；
   e = 63 - clz，f 归一化后的高 9 位（256..511）查 256 项表得 Q8 尾数。 */
static int32_t Fft_Log2Q8(uint64_t v)
{
    uint8_t  clz = Fft_Clz64(v);
    uint8_t  e   = (uint8_t)(63U - clz);
    uint32_t m   = (uint32_t)((v << (uint32_t)(63U - e)) >> 55U);

    return ((int32_t)e << 8) + (int32_t)k_fft_log2_q8[m - 256U];
}

/* 位倒序：DIT 蝶形之前把输入按二进制位反转重排 */
static void Fft_BitReverse(int32_t *re, int32_t *im)
{
    uint16_t i;
    uint16_t j = 0U;

    for (i = 1U; i < FFT_SIZE; i++)
    {
        uint16_t k = (uint16_t)(FFT_SIZE >> 1U);
        int32_t  t;

        while ((j & k) != 0U)
        {
            j = (uint16_t)(j ^ k);
            k = (uint16_t)(k >> 1U);
        }
        j = (uint16_t)(j ^ k);

        if (i < j)
        {
            t     = re[i];
            re[i] = re[j];
            re[j] = t;

            t     = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
}

/* radix-2 蝶形（原地）。
   旋转因子 w = cos - j*sin（Q15），所以
     t = b * w = (br*c + bi*s) + j*(bi*c - br*s)，再 >>15 还原 Q15。 */
static void Fft_Butterflies(int32_t *re, int32_t *im)
{
    uint16_t len;
    uint16_t half;
    uint16_t step;
    uint16_t i;
    uint16_t j;

    for (len = 2U; len <= FFT_SIZE; len = (uint16_t)(len << 1U))
    {
        half = (uint16_t)(len >> 1U);
        step = (uint16_t)(FFT_SIZE / len);

        for (i = 0U; i < FFT_SIZE; i = (uint16_t)(i + len))
        {
            uint16_t m = 0U;

            for (j = 0U; j < half; j++)
            {
                int32_t c  = (int32_t)k_fft_cos_q15[m];
                int32_t s  = (int32_t)k_fft_sin_q15[m];
                int32_t ar = re[i + j];
                int32_t ai = im[i + j];
                int32_t br = re[i + j + half];
                int32_t bi = im[i + j + half];
                int32_t tr = (int32_t)((((int64_t)br * c) + ((int64_t)bi * s)) >> 15);
                int32_t ti = (int32_t)((((int64_t)bi * c) - ((int64_t)br * s)) >> 15);

                re[i + j]        = ar + tr;
                im[i + j]        = ai + ti;
                re[i + j + half] = ar - tr;
                im[i + j + half] = ai - ti;

                m = (uint16_t)(m + step);
            }
        }
    }
}

void Fft_Analyze(const int32_t *samples, uint16_t count, Fft_Result *out)
{
    static int32_t s_re[FFT_SIZE];
    static int32_t s_im[FFT_SIZE];

    uint16_t start;
    uint16_t i;
    uint64_t pmax = 0U;
    uint16_t peak = 1U;
    int32_t  ref_q8;
    int64_t  sum = 0;
    int32_t  mean;

    if ((samples == 0) || (out == 0) || (count < FFT_SIZE))
    {
        return;
    }

    /* 取中间 256 点：触发点固定在第 160 点，两侧各留 32 点余量，
       频谱稳定且不引入帧间相位跳变。 */
    start = (uint16_t)((count - FFT_SIZE) / 2U);

    /* 先去直流再乘窗：否则直流会泄漏到 bin1/bin2，看起来像一条低频信号 */
    for (i = 0U; i < FFT_SIZE; i++)
    {
        sum += (int64_t)samples[start + i];
    }
    mean = (int32_t)(sum / (int64_t)FFT_SIZE);

    for (i = 0U; i < FFT_SIZE; i++)
    {
        int64_t v = (int64_t)(samples[start + i] - mean) * (int64_t)k_fft_hann_q15[i];

        s_re[i] = (int32_t)(v >> 15);
        s_im[i] = 0;
    }

    Fft_BitReverse(s_re, s_im);
    Fft_Butterflies(s_re, s_im);

    /* 找最强谱线：跳过 bin0（直流 / 偏置），只统计交流谱线 */
    for (i = 1U; i < FFT_BINS; i++)
    {
        int64_t p = (int64_t)s_re[i] * s_re[i] + (int64_t)s_im[i] * s_im[i];

        if ((uint64_t)p > pmax)
        {
            pmax = (uint64_t)p;
            peak = i;
        }
    }
    if (pmax == 0U)
    {
        pmax = 1U;
    }

    ref_q8 = Fft_Log2Q8(pmax);

    out->bins     = (uint16_t)FFT_BINS;
    out->peak_bin = (uint8_t)peak;
    out->mag_db[0] = -60;                 /* 直流分量不显示 */

    for (i = 1U; i < FFT_BINS; i++)
    {
        int64_t p = (int64_t)s_re[i] * s_re[i] + (int64_t)s_im[i] * s_im[i];
        int32_t db;

        if (p <= 1)
        {
            db = -60;
        }
        else
        {
            /* dB = 10*log10(P/Pmax) = 3.0103 * (log2(P) - log2(Pmax))。
               3.0103 的 Q8 值是 771，所以 (delta_q8 * 771) / 65536 就是整数 dB。 */
            int32_t delta_q8 = Fft_Log2Q8((uint64_t)p) - ref_q8;

            db = (int32_t)(((int64_t)delta_q8 * 771) / 65536);

            if (db < -60)
            {
                db = -60;
            }
            if (db > 0)
            {
                db = 0;
            }
        }

        out->mag_db[i] = (int16_t)db;
    }
}
