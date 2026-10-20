/**
  * @file    StatusBar.c
  * @brief   状态栏绘制（320 x 22）
  *
  * 显示内容（从左到右）
  * -------------------
  *   CH1 1.00V/div | 20.0kSa/s | 2.00ms/div | DCx1 | RUN | LIMIT
  *
  * 所有数字都用整数定点格式化，不依赖 printf 浮点支持。
  * 本文件只画界面，不保存任何状态；状态由 App 层通过 StatusBar_Info 传入。
  */

#include "ScopeView.h"

#include <stdio.h>

/* 状态栏文字基线：22 px 高、字高 9 px，居中偏移 6 */
#define STATUS_TEXT_Y   ((uint16_t)(VIEW_STATUS_Y + ((VIEW_STATUS_H - FONT_SMALL_HEIGHT) / 2U)))

#define STATUS_X_CH     4U
#define STATUS_X_RATE   86U
#define STATUS_X_TIME   146U
#define STATUS_X_MODE   210U
#define STATUS_X_RUN    238U
#define STATUS_X_LIMIT  260U

/* 把 V/div（mV）格式化为 "500mV/div" 或 "1.00V/div" */
static void Status_FormatVdiv(uint16_t mv, char *buf, size_t n)
{
    if (mv >= 1000U)
    {
        (void)snprintf(buf, n, "%u.%02uV/div",
                       (unsigned)(mv / 1000U),
                       (unsigned)((mv % 1000U) / 10U));
    }
    else
    {
        (void)snprintf(buf, n, "%umV/div", (unsigned)mv);
    }
}

/* 采样率：>= 1 MSa/s 用 M，>= 1 kSa/s 用 k，否则原样 */
static void Status_FormatRate(uint32_t rate, char *buf, size_t n)
{
    if (rate >= 1000000UL)
    {
        (void)snprintf(buf, n, "%lu.%02luMSa/s",
                       (unsigned long)(rate / 1000000UL),
                       (unsigned long)((rate % 1000000UL) / 10000UL));
    }
    else if (rate >= 1000UL)
    {
        (void)snprintf(buf, n, "%lu.%01lukSa/s",
                       (unsigned long)(rate / 1000UL),
                       (unsigned long)((rate % 1000UL) / 100UL));
    }
    else
    {
        (void)snprintf(buf, n, "%luSa/s", (unsigned long)rate);
    }
}

/* 时间/格：us -> "200us/div" / "1.00ms/div" / "10.0ms/div" */
static void Status_FormatTdiv(uint32_t us, char *buf, size_t n)
{
    if (us >= 10000UL)
    {
        (void)snprintf(buf, n, "%lu.%01lums/div",
                       (unsigned long)(us / 1000UL),
                       (unsigned long)((us % 1000UL) / 100UL));
    }
    else if (us >= 1000UL)
    {
        (void)snprintf(buf, n, "%lu.%02lums/div",
                       (unsigned long)(us / 1000UL),
                       (unsigned long)((us % 1000UL) / 10UL));
    }
    else
    {
        (void)snprintf(buf, n, "%luus/div", (unsigned long)us);
    }
}

void StatusBar_Draw(const StatusBar_Info *info)
{
    char     buf[24];
    uint16_t x;
    uint16_t ch_color;

    if (info == 0)
    {
        return;
    }

    ch_color = (info->channel == 2U) ? VIEW_COLOR_CH2 : VIEW_COLOR_CH1;

    /* 底板 */
    Lcd_FillRect(0U, VIEW_STATUS_Y, VIEW_W, VIEW_STATUS_H, VIEW_COLOR_BAR_BG);

    /* 通道号（用通道色，便于一眼看出当前选中哪个通道） */
    (void)snprintf(buf, sizeof(buf), "CH%u ", (unsigned)info->channel);
    (void)Lcd_DrawString(STATUS_X_CH, STATUS_TEXT_Y, buf, FONT_SMALL,
                         ch_color, VIEW_COLOR_BAR_BG);

    /* 该通道的 V/div（紧接在 "CHx " 之后） */
    Status_FormatVdiv(info->vdiv_mv, buf, sizeof(buf));
    x = (uint16_t)(STATUS_X_CH + (4U * FONT_SMALL_WIDTH));
    (void)Lcd_DrawString(x, STATUS_TEXT_Y, buf, FONT_SMALL,
                         VIEW_COLOR_TEXT, VIEW_COLOR_BAR_BG);

    /* 采样率 */
    Status_FormatRate(info->sample_rate, buf, sizeof(buf));
    (void)Lcd_DrawString(STATUS_X_RATE, STATUS_TEXT_Y, buf, FONT_SMALL,
                         VIEW_COLOR_TEXT, VIEW_COLOR_BAR_BG);

    /* Time/div */
    Status_FormatTdiv(info->tdiv_us, buf, sizeof(buf));
    (void)Lcd_DrawString(STATUS_X_TIME, STATUS_TEXT_Y, buf, FONT_SMALL,
                         VIEW_COLOR_TEXT, VIEW_COLOR_BAR_BG);

    /* 耦合 + 前端量程：DC W / AC S
       W = 大量程（跳线乙不插），S = 小量程（跳线乙插 MID）。
       软件检测不到跳线，只能靠用户把跳线与这个提示保持一致。 */
    (void)snprintf(buf, sizeof(buf), "%s %c",
                   (info->ac_dc != 0U) ? "AC" : "DC",
                   (info->range != 0U) ? 'S' : 'W');
    (void)Lcd_DrawString(STATUS_X_MODE, STATUS_TEXT_Y, buf, FONT_SMALL,
                         VIEW_COLOR_LABEL, VIEW_COLOR_BAR_BG);

    /* 运行状态 */
    (void)Lcd_DrawString(STATUS_X_RUN, STATUS_TEXT_Y,
                         (info->running != 0U) ? "RUN" : "HOLD",
                         FONT_SMALL,
                         (info->running != 0U) ? VIEW_COLOR_RUN : VIEW_COLOR_HOLD,
                         VIEW_COLOR_BAR_BG);

    /* 采样率钳位提示 */
    if (info->rate_limited != 0U)
    {
        (void)Lcd_DrawString(STATUS_X_LIMIT, STATUS_TEXT_Y, "LIMIT", FONT_SMALL,
                             VIEW_COLOR_SELECT, VIEW_COLOR_BAR_BG);
    }
}
