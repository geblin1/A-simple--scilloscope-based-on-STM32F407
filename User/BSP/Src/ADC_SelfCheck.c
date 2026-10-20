/**
  ******************************************************************************
  * @file    ADC_SelfCheck.c
  * @brief   模拟前端偏置改造自检（纯串口输出，不依赖 LCD）
  ******************************************************************************
  * 设计说明：
  *   - 只用 ADC_Drv 的公共接口（ADC_Drv_ReadOnce），不碰 HAL/寄存器；
  *   - 每 500 ms 采样 64 次，用 min/max 的差值看噪声；
  *   - 因为程序不知道跳线乙插没插，所以把同一个码值按"大量程"和"小量程"
  *     各折算一次，接的是 GND 时必有一行接近 0 V，据此判断实际是哪个档位。
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include <stdio.h>
#include "ADC_Drv.h"
#include "ADC_SelfCheck.h"

/* Private define ------------------------------------------------------------*/
#define SELFCHECK_SAMPLES        64U     /* 每次打印前连续采样的次数 */
#define SELFCHECK_PERIOD_MS      500U    /* 打印间隔 */
#define SELFCHECK_VREF_MV        3300L   /* 假定 VDDA = 3.300 V */
#define SELFCHECK_FULL           4095L   /* 12 位满量程 */

/* 改造后的理论值，见 FRONTEND_BIAS_DESIGN.md 第七节（已用实测校准） */
#define SELFCHECK_WIDE_ZERO_MV   1571L   /* 大量程：输入 0 V 时引脚电压 */
#define SELFCHECK_WIDE_GAIN_NUM  21L     /* 大量程增益 1/21 -> 折算回输入要乘 21 */
#define SELFCHECK_WIDE_GAIN_DEN  1L
#define SELFCHECK_SMALL_ZERO_MV  1065L   /* 小量程：输入 0 V 时引脚电压 */
#define SELFCHECK_SMALL_GAIN_NUM 2818L   /* 小量程增益 1/2.818 -> 折算回输入要乘 2.818 */
#define SELFCHECK_SMALL_GAIN_DEN 1000L

/* Private functions ---------------------------------------------------------*/

/** 码值 -> 引脚电压（mV） */
static uint32_t SelfCheck_PinMv(uint16_t raw)
{
    return ((uint32_t)raw * (uint32_t)SELFCHECK_VREF_MV) / (uint32_t)SELFCHECK_FULL;
}

/** 码值 -> 输入电压（mV），按给定的零点与衰减折算 */
static int32_t SelfCheck_InputMv(uint16_t raw, int32_t zero_mv, int32_t num, int32_t den)
{
    int32_t pin = (int32_t)SelfCheck_PinMv(raw);

    return ((pin - zero_mv) * num) / den;
}

/** 打印带符号的电压，两位小数（10 mV 分辨率） */
static void SelfCheck_PrintVolt(int32_t mv)
{
    if (mv < 0)
    {
        mv = -mv;
        (void)printf("-%ld.%02ld V", (long)(mv / 1000), (long)((mv % 1000) / 10));
    }
    else
    {
        (void)printf("+%ld.%02ld V", (long)(mv / 1000), (long)((mv % 1000) / 10));
    }
}

/** 根据码值给一句人话判断 */
static const char *SelfCheck_Hint(uint16_t raw)
{
    if (raw >= 4088U)
    {
        return "pin at VCC (saturated) -> bias missing or short to 3.3V";
    }
    if (raw >= 3000U)
    {
        return "pin pulled hard to VCC (short to 3.3V on the breadboard?)";
    }
    if (raw <= 30U)
    {
        return "pin at 0V -> bias resistor missing or shorted";
    }
    if ((raw >= 2005U) && (raw <= 2090U))
    {
        return "pin ~1.65V -> bias divider OK, input floating";
    }
    if ((raw >= 1900U) && (raw <= 2004U))
    {
        return "matches WIDE zero (1950) -> input at GND, WIDE mode";
    }
    if ((raw >= 2095U) && (raw <= 2195U))
    {
        return "matches WIDE 3.3V point (2145) -> input at 3.3V, WIDE mode";
    }
    if ((raw >= 1271U) && (raw <= 1371U))
    {
        return "matches SMALL zero (1321) -> input at GND, SMALL mode";
    }
    if ((raw >= 1711U) && (raw <= 1811U))
    {
        return "matches SMALL 1.0V point (1761) -> input at 1.0V, SMALL mode";
    }
    if ((raw >= 2724U) && (raw <= 2824U))
    {
        return "matches SMALL 3.3V point (2774) -> input at 3.3V, SMALL mode";
    }
    return "no calibration point matched - check wiring / jumper";
}

/** 偏置网络结论（只看"悬空时是否为 1.65 V"这一条） */
static const char *SelfCheck_BiasVerdict(uint16_t raw)
{
    if (raw <= 30U)
    {
        return "FAIL: pin stuck at 0V -> check the new 1k to 3.3V";
    }
    if (raw >= 4088U)
    {
        return "FAIL: pin stuck at VCC -> check the new 1k to MID/GND";
    }
    if ((raw >= 2005U) && (raw <= 2090U))
    {
        return "OK: bias divider works (input floating)";
    }
    return "input is driving the pin (correct if a source is connected)";
}

/* Exported functions --------------------------------------------------------*/

void ADC_SelfCheck_Run(void)
{
    static uint8_t  s_banner_done = 0U;
    static uint8_t  s_tick_valid  = 0U;
    static uint32_t s_next_tick   = 0U;

    uint32_t now = HAL_GetTick();
    uint32_t i;
    uint32_t lo1 = 4095U;
    uint32_t hi1 = 0U;
    uint32_t lo2 = 4095U;
    uint32_t hi2 = 0U;
    uint16_t c1 = 0U;
    uint16_t c2 = 0U;
    uint8_t  read_fail = 0U;

    if (s_banner_done == 0U)
    {
        s_banner_done = 1U;
        (void)printf("\r\n");
        (void)printf("==== FRONTEND SELF-CHECK | CH1=PA0(IN0) CH2=PA1(IN1) | 12bit | assume VDDA=3.300V ====\r\n");
        (void)printf("expect WIDE  (jumper2 open): float 2048 | GND 1950 | 1.0V 2009 | 3.3V 2145\r\n");
        (void)printf("expect SMALL (jumper2 MID ) : float 2048 | GND 1321 | 1.0V 1761 | 3.3V 2774\r\n");
        (void)printf("CH1 = PA0(IN0) with bias, CH2 = PA1(IN1) with bias\r\n");
        (void)printf("---------------------------------------------------------------------------------------\r\n");
    }

    /* 非阻塞节流 */
    if ((s_tick_valid != 0U) && ((now - s_next_tick) < SELFCHECK_PERIOD_MS))
    {
        return;
    }
    s_next_tick  = now;
    s_tick_valid = 1U;

    /* 连续采样，取 min/max 看噪声 */
    for (i = 0U; i < SELFCHECK_SAMPLES; i++)
    {
        ADC_Drv_Status st = ADC_Drv_ReadOnce(&c1, &c2);

        if (st != ADC_DRV_OK)
        {
            /* 打印具体状态码：0=OK 1=BUSY 2=PARAM 3=TIMEOUT 4=ERROR */
            (void)printf("READ FAIL: ADC_Drv_ReadOnce() status=%u\r\n", (unsigned)st);
            read_fail = 1U;
            break;
        }
        if ((uint32_t)c1 < lo1) { lo1 = (uint32_t)c1; }
        if ((uint32_t)c1 > hi1) { hi1 = (uint32_t)c1; }
        if ((uint32_t)c2 < lo2) { lo2 = (uint32_t)c2; }
        if ((uint32_t)c2 > hi2) { hi2 = (uint32_t)c2; }
    }

    if (read_fail != 0U)
    {
        return;
    }

    /* ---- CH1 ---- */
    (void)printf("CH1 raw %4u  min %4lu  max %4lu  pin %4lu mV\r\n",
                 (unsigned)c1, (unsigned long)lo1, (unsigned long)hi1,
                 (unsigned long)SelfCheck_PinMv(c1));

    (void)printf("    WIDE  : Vin = ");
    SelfCheck_PrintVolt(SelfCheck_InputMv(c1, SELFCHECK_WIDE_ZERO_MV,
                                          SELFCHECK_WIDE_GAIN_NUM, SELFCHECK_WIDE_GAIN_DEN));
    (void)printf("     SMALL : Vin = ");
    SelfCheck_PrintVolt(SelfCheck_InputMv(c1, SELFCHECK_SMALL_ZERO_MV,
                                          SELFCHECK_SMALL_GAIN_NUM, SELFCHECK_SMALL_GAIN_DEN));
    (void)printf("\r\n");

    (void)printf("    hint  : %s\r\n", SelfCheck_Hint(c1));
    if ((hi1 - lo1) > 15U)
    {
        (void)printf("    noise : spread %lu counts - check the 100nF decoupling on 3.3V\r\n",
                     (unsigned long)(hi1 - lo1));
    }

    /* ---- CH2 ---- */
    (void)printf("CH2 raw %4u  min %4lu  max %4lu  pin %4lu mV\r\n",
                 (unsigned)c2, (unsigned long)lo2, (unsigned long)hi2,
                 (unsigned long)SelfCheck_PinMv(c2));

    (void)printf("    WIDE  : Vin = ");
    SelfCheck_PrintVolt(SelfCheck_InputMv(c2, SELFCHECK_WIDE_ZERO_MV,
                                          SELFCHECK_WIDE_GAIN_NUM, SELFCHECK_WIDE_GAIN_DEN));
    (void)printf("     SMALL : Vin = ");
    SelfCheck_PrintVolt(SelfCheck_InputMv(c2, SELFCHECK_SMALL_ZERO_MV,
                                          SELFCHECK_SMALL_GAIN_NUM, SELFCHECK_SMALL_GAIN_DEN));
    (void)printf("\r\n");

    (void)printf("    hint  : %s\r\n", SelfCheck_Hint(c2));
    if ((hi2 - lo2) > 15U)
    {
        (void)printf("    noise : spread %lu counts - check the 100nF decoupling on 3.3V\r\n",
                     (unsigned long)(hi2 - lo2));
    }

    /* ---- 总结 ---- */
    (void)printf("verdict : CH1 %s | CH2 %s\r\n",
                 SelfCheck_BiasVerdict(c1),
                 SelfCheck_BiasVerdict(c2));
    (void)printf("\r\n");
}
