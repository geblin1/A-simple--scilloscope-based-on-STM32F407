/**
  * @file    Frontend.c
  * @brief   模拟前端标定实现
  *
  * 标定表的来历（详见 FRONTEND_BIAS_DESIGN.md 第七节）
  * ------------------------------------------------
  * ADC 引脚是一个三支路节点：1 kΩ 到 3.3 V、1 kΩ 到 GND、Rs 到信号源。
  * 解基尔霍夫电流定律得
  *
  *     V_pin = 1.65 V + (V_in − 1.65 V) × g ,   g = 500 / (500 + Rs)
  *
  * 于是   零点（V_in = 0）= 1.65 V × (1 − g)
  *        增益倒数 = 1 / g
  *
  * 大量程 Rs = 10 kΩ  -> g = 1/21  , 零点 1571 mV , 码值 1950
  * 小量程 Rs = 909 Ω   -> g = 1/2.818, 零点 1065 mV , 码值 1321
  *
  * 码值 1950 / 1321 都恰好是整数，所以表里直接存整数码值，不需要小数。
  */

#include "Frontend.h"

/* 一种量程的标定参数 */
typedef struct
{
    int32_t zero_raw;      /* 输入 0 V 时的理论码值 */
    int32_t zero_mv;       /* 输入 0 V 时的引脚电压 */
    int32_t gain_num;      /* 输入 mV = (引脚 mV − 零点) × gain_num / gain_den */
    int32_t gain_den;
    int32_t min_mv;        /* 可测下限（负） */
    int32_t max_mv;        /* 可测上限（正） */
    int32_t lsb_milli_mv;  /* 每码值对应 0.001 mV */
} Frontend_Cal;

static const Frontend_Cal k_frontend_cal[2] =
{
    /* FRONTEND_RANGE_WIDE：跳线乙不插，Rs = 10 kΩ */
    { 1950, 1571, 21,   1,    -33000, 36300, 16923 },
    /* FRONTEND_RANGE_SMALL：跳线乙插 MID，Rs = 909 Ω */
    { 1321, 1065, 2818, 1000, -3000,  6300,  2271  }
};

/* 上电默认小量程：与默认 V/div（500 mV/div）对应 */
static Frontend_Range s_range = FRONTEND_RANGE_SMALL;

static const Frontend_Cal *Frontend_Cal_Of(void)
{
    uint8_t idx = (s_range == FRONTEND_RANGE_WIDE) ? 0U : 1U;

    return &k_frontend_cal[idx];
}

void Frontend_SetRange(Frontend_Range range)
{
    s_range = (range == FRONTEND_RANGE_WIDE) ? FRONTEND_RANGE_WIDE : FRONTEND_RANGE_SMALL;
}

Frontend_Range Frontend_GetRange(void)
{
    return s_range;
}

const char *Frontend_RangeLetter(void)
{
    return (s_range == FRONTEND_RANGE_WIDE) ? "W" : "S";
}

const char *Frontend_RangeName(void)
{
    return (s_range == FRONTEND_RANGE_WIDE) ? "WIDE" : "SMALL";
}

const char *Frontend_JumperHint(void)
{
    return (s_range == FRONTEND_RANGE_WIDE) ? "OPEN" : "MID";
}

int32_t Frontend_RawToMv(uint16_t raw)
{
    const Frontend_Cal *cal = Frontend_Cal_Of();
    int64_t codes = (int64_t)((int32_t)raw - cal->zero_raw);

    /* 输入 mV = (码值 − 零点) × VREF/FULL × 增益倒数
       中间用 int64：小量程时 4095 × 3300 × 2818 ≈ 3.8e10，超过 int32。 */
    return (int32_t)(codes * (int64_t)FRONTEND_VREF_MV * (int64_t)cal->gain_num
                     / ((int64_t)FRONTEND_RAW_FULL * (int64_t)cal->gain_den));
}

int32_t Frontend_ZeroRaw(void)
{
    return Frontend_Cal_Of()->zero_raw;
}

int32_t Frontend_ZeroMv(void)
{
    return Frontend_Cal_Of()->zero_mv;
}

int32_t Frontend_MinMv(void)
{
    return Frontend_Cal_Of()->min_mv;
}

int32_t Frontend_MaxMv(void)
{
    return Frontend_Cal_Of()->max_mv;
}

int32_t Frontend_MvPerLsbMilli(void)
{
    return Frontend_Cal_Of()->lsb_milli_mv;
}
