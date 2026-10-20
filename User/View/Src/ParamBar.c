/**
  * @file    ParamBar.c
  * @brief   参数栏绘制（320 x 28）
  *
  * 显示内容（从左到右）
  * -------------------
  *   F 1.00kHz | T 1.00ms | Vpp 3.30V | RMS 1.17V | DC 1.65V
  *
  * 数值全部以整数定点传入：
  *   freq_mhz  —— 频率，单位 0.001 Hz
  *   period_us —— 周期，单位 us
  *   vpp/rms/mean —— 电压，单位 mV
  * 这样 View 层不需要浮点，也不必打开 printf 的浮点支持。
  *
  * 刷新策略
  * --------
  * 整条底板重画要 320 x 28 x 2 = 17.9 KB，在 10.5 Mbit/s 的 SPI 上就是 14 ms，
  * 而参数栏每帧真正变的通常只有一两个字段。所以这里做字段级增量：
  *   - 每个字段先格式化成字符串，与上一帧比较，相同就整块跳过；
  *   - 内容变了才重画该字段，并把新文字没盖到的尾巴用背景色清干净。
  */

#include "ScopeView.h"

#include <stdio.h>
#include <string.h>

/* 参数栏文字基线：28 px 高、字高 9 px，居中偏移 9 */
#define PARAM_TEXT_Y    ((uint16_t)(VIEW_PARAM_Y + ((VIEW_PARAM_H - FONT_SMALL_HEIGHT) / 2U)))

#define PARAM_X_F       4U
#define PARAM_X_T       66U
#define PARAM_X_VPP     128U
#define PARAM_X_RMS     190U
#define PARAM_X_DC      252U

#define PARAM_FIELDS    5U
#define PARAM_TEXT_MAX  20U

/* 频率：mHz 输入 -> "1.00kHz" / "500Hz" / "---" */
static void Param_FormatFreq(int32_t freq_mhz, char *buf, size_t n)
{
    if (freq_mhz <= 0)
    {
        (void)snprintf(buf, n, "---");
    }
    else if (freq_mhz >= 1000000)
    {
        (void)snprintf(buf, n, "%ld.%02ldkHz",
                       (long)(freq_mhz / 1000000),
                       (long)((freq_mhz % 1000000) / 10000));
    }
    else if (freq_mhz >= 1000)
    {
        (void)snprintf(buf, n, "%ldHz", (long)(freq_mhz / 1000));
    }
    else
    {
        (void)snprintf(buf, n, "%ldmHz", (long)freq_mhz);
    }
}

/* 周期：us 输入 -> "1.00ms" / "200us" / "---" */
static void Param_FormatPeriod(int32_t period_us, char *buf, size_t n)
{
    if (period_us <= 0)
    {
        (void)snprintf(buf, n, "---");
    }
    else if (period_us >= 1000)
    {
        (void)snprintf(buf, n, "%ld.%02ldms",
                       (long)(period_us / 1000),
                       (long)((period_us % 1000) / 10));
    }
    else
    {
        (void)snprintf(buf, n, "%ldus", (long)period_us);
    }
}

/* 电压：mV 输入（可带符号） -> "3.30V" / "512mV" / "---" */
static void Param_FormatVolt(int32_t mv, char *buf, size_t n, uint8_t valid)
{
    int32_t mag;
    const char *sign;

    if (valid == 0U)
    {
        (void)snprintf(buf, n, "---");
        return;
    }

    sign = (mv < 0) ? "-" : "";
    mag  = (mv < 0) ? -mv : mv;

    if (mag >= 1000)
    {
        (void)snprintf(buf, n, "%s%ld.%02ldV", sign,
                       (long)(mag / 1000), (long)((mag % 1000) / 10));
    }
    else
    {
        (void)snprintf(buf, n, "%s%ldmV", sign, (long)mag);
    }
}

/* ------------------------------------------------------------------ */
/* 字段级增量刷新                                                      */
/* ------------------------------------------------------------------ */

/* 每个字段的起点，以及"到下一个字段为止"的可用宽度 */
static const uint16_t k_field_x[PARAM_FIELDS] =
{
    PARAM_X_F, PARAM_X_T, PARAM_X_VPP, PARAM_X_RMS, PARAM_X_DC
};

static const uint16_t k_field_w[PARAM_FIELDS] =
{
    (uint16_t)(PARAM_X_T - PARAM_X_F),
    (uint16_t)(PARAM_X_VPP - PARAM_X_T),
    (uint16_t)(PARAM_X_RMS - PARAM_X_VPP),
    (uint16_t)(PARAM_X_DC - PARAM_X_RMS),
    (uint16_t)(VIEW_W - PARAM_X_DC)
};

static char    s_field_prev[PARAM_FIELDS][PARAM_TEXT_MAX];
static uint8_t s_field_valid;                  /* 1 = 上一次绘制的内容还有效 */

/* 与上一帧内容相同返回 0（跳过），不同则记录并返回 1（需要重画） */
static uint8_t Param_FieldChanged(uint8_t idx, const char *text)
{
    if ((s_field_valid != 0U) &&
        (strncmp(s_field_prev[idx], text, PARAM_TEXT_MAX) == 0))
    {
        return 0U;
    }

    (void)snprintf(s_field_prev[idx], sizeof(s_field_prev[idx]), "%s", text);
    return 1U;
}

/* 画一个字段：标签用 LABEL 色、数值用 TEXT 色，多余部分用背景色清掉 */
static void Param_DrawField(uint8_t idx, const char *label, const char *value)
{
    uint16_t x    = k_field_x[idx];
    uint16_t w    = k_field_w[idx];
    uint16_t lw   = (uint16_t)(strlen(label) * FONT_SMALL_WIDTH);
    uint16_t used = (uint16_t)(lw + Lcd_TextWidth(FONT_SMALL, value));

    if (used < w)
    {
        Lcd_FillRect((uint16_t)(x + used), PARAM_TEXT_Y, (uint16_t)(w - used),
                     (uint16_t)FONT_SMALL_HEIGHT, VIEW_COLOR_BAR_BG);
    }

    (void)Lcd_DrawString(x, PARAM_TEXT_Y, label, FONT_SMALL,
                         VIEW_COLOR_LABEL, VIEW_COLOR_BAR_BG);
    (void)Lcd_DrawString((uint16_t)(x + lw), PARAM_TEXT_Y, value, FONT_SMALL,
                         VIEW_COLOR_TEXT, VIEW_COLOR_BAR_BG);
}

void ParamBar_Draw(const ParamBar_Info *info)
{
    char    v_freq[PARAM_TEXT_MAX];
    char    v_period[PARAM_TEXT_MAX];
    char    v_vpp[PARAM_TEXT_MAX];
    char    v_rms[PARAM_TEXT_MAX];
    char    v_mean[PARAM_TEXT_MAX];
    uint8_t valid;

    if (info == 0)
    {
        return;
    }

    valid = info->valid;

    if (valid != 0U)
    {
        Param_FormatFreq(info->freq_mhz, v_freq, sizeof(v_freq));
        Param_FormatPeriod(info->period_us, v_period, sizeof(v_period));
    }
    else
    {
        (void)snprintf(v_freq, sizeof(v_freq), "---");
        (void)snprintf(v_period, sizeof(v_period), "---");
    }

    Param_FormatVolt(info->vpp_mv, v_vpp, sizeof(v_vpp), valid);
    Param_FormatVolt(info->rms_mv, v_rms, sizeof(v_rms), valid);
    Param_FormatVolt(info->mean_mv, v_mean, sizeof(v_mean), valid);

    /* 第一次绘制（或整屏刚重画过）才铺整条底板 */
    if (s_field_valid == 0U)
    {
        Lcd_FillRect(0U, VIEW_PARAM_Y, VIEW_W, VIEW_PARAM_H, VIEW_COLOR_BAR_BG);
    }

    if (Param_FieldChanged(0U, v_freq) != 0U)
    {
        Param_DrawField(0U, "F ", v_freq);
    }
    if (Param_FieldChanged(1U, v_period) != 0U)
    {
        Param_DrawField(1U, "T ", v_period);
    }
    if (Param_FieldChanged(2U, v_vpp) != 0U)
    {
        Param_DrawField(2U, "Vpp ", v_vpp);
    }
    if (Param_FieldChanged(3U, v_rms) != 0U)
    {
        Param_DrawField(3U, "RMS ", v_rms);
    }
    if (Param_FieldChanged(4U, v_mean) != 0U)
    {
        Param_DrawField(4U, "DC ", v_mean);
    }

    s_field_valid = 1U;
}
