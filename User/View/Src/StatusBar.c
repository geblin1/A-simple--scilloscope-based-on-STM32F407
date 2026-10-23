/**
  * @file    StatusBar.c
  * @brief   状态栏绘制（320 x 22）
  *
  * 显示内容（从左到右）
  * -------------------
  *   CH1 500m 2.0V | 20.0kSa/s | 2.00ms/div | S W | RUN | LIMIT
  *
  * 两路 V/div 并排显示：CH1 的值靠左（通道1色）、CH2 的值右对齐（通道2色）；
  * 量程字母也是两路各一个（CH1 色 / CH2 色），分别提示两路跳线乙应插的位置。
  * 耦合方式（AC/DC）由跳线甲决定、软件测不到，因此状态栏不显示。
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

/* 两路 V/div 并排时的紧凑格式："500m" / "1.0V" / "10V"。
   最长 4 个字符（24 px），两个值放得进 CH 与采样率之间的 58 px。 */
static void Status_FormatVdivShort(uint16_t mv, char *buf, size_t n)
{
    if (mv >= 10000U)
    {
        (void)snprintf(buf, n, "%uV", (unsigned)(mv / 1000U));
    }
    else if (mv >= 1000U)
    {
        (void)snprintf(buf, n, "%u.%uV",
                       (unsigned)(mv / 1000U),
                       (unsigned)((mv % 1000U) / 100U));
    }
    else
    {
        (void)snprintf(buf, n, "%um", (unsigned)mv);
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

/* 频谱模式的横轴：8 格 = 0 .. 采样率/2，所以每格 = 采样率/16。
   例：40 kSa/s -> "2.50kHz/div"；1 kSa/s -> "62.5Hz/div"。
   提示：字段宽 10 个字符，所有分支都控制在这个宽度内。 */
static void Status_FormatFreqDiv(uint32_t sps, char *buf, size_t n)
{
    uint32_t hz   = sps / 16U;
    uint32_t frac = ((sps % 16U) * 10U) / 16U;   /* 0.1 Hz 精度 */

    if (hz >= 10000UL)
    {
        (void)snprintf(buf, n, "%lu.%01lukHz/div",
                       (unsigned long)(hz / 1000UL),
                       (unsigned long)((hz % 1000UL) / 100UL));
    }
    else if (hz >= 1000UL)
    {
        (void)snprintf(buf, n, "%lu.%02lukHz/div",
                       (unsigned long)(hz / 1000UL),
                       (unsigned long)((hz % 1000UL) / 10UL));
    }
    else if (frac != 0UL)
    {
        (void)snprintf(buf, n, "%lu.%luHz/div",
                       (unsigned long)hz, (unsigned long)frac);
    }
    else
    {
        (void)snprintf(buf, n, "%luHz/div", (unsigned long)hz);
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

    /* 两路 V/div：CH1 靠左、CH2 右对齐到采样率左边，各用通道色区分；
       频谱模式下这一格显示 FFT 标记（谱线的 dB 刻度与 V/div 无关）。 */
    x = (uint16_t)(STATUS_X_CH + (4U * FONT_SMALL_WIDTH));
    if (info->fft != 0U)
    {
        (void)Lcd_DrawString(x, STATUS_TEXT_Y, "FFT", FONT_SMALL,
                             VIEW_COLOR_LABEL, VIEW_COLOR_BAR_BG);
    }
    else
    {
        char     vbuf[8];
        uint16_t w2;

        Status_FormatVdivShort(info->vdiv_mv1, vbuf, sizeof(vbuf));
        (void)Lcd_DrawString(x, STATUS_TEXT_Y, vbuf, FONT_SMALL,
                             VIEW_COLOR_CH1, VIEW_COLOR_BAR_BG);

        Status_FormatVdivShort(info->vdiv_mv2, vbuf, sizeof(vbuf));
        w2 = Lcd_TextWidth(FONT_SMALL, vbuf);
        (void)Lcd_DrawString((uint16_t)(STATUS_X_RATE - 2U - w2), STATUS_TEXT_Y,
                             vbuf, FONT_SMALL, VIEW_COLOR_CH2, VIEW_COLOR_BAR_BG);
    }

    /* 采样率 */
    Status_FormatRate(info->sample_rate, buf, sizeof(buf));
    (void)Lcd_DrawString(STATUS_X_RATE, STATUS_TEXT_Y, buf, FONT_SMALL,
                         VIEW_COLOR_TEXT, VIEW_COLOR_BAR_BG);

    /* Time/div；频谱模式下这一格改成"频率/格" */
    if (info->fft != 0U)
    {
        Status_FormatFreqDiv(info->sample_rate, buf, sizeof(buf));
    }
    else
    {
        Status_FormatTdiv(info->tdiv_us, buf, sizeof(buf));
    }
    (void)Lcd_DrawString(STATUS_X_TIME, STATUS_TEXT_Y, buf, FONT_SMALL,
                         VIEW_COLOR_TEXT, VIEW_COLOR_BAR_BG);

    /* 两路前端量程字母：CH1 的靠左、CH2 的靠右，各用通道色。
       W = 大量程（跳线乙不插），S = 小量程（跳线乙插 MID）。
       软件检测不到跳线，只能靠用户把两路跳线分别与这两个字母保持一致。
       耦合方式（AC/DC）由跳线甲决定、软件同样测不到，故不再显示。 */
    (void)Lcd_DrawString(STATUS_X_MODE, STATUS_TEXT_Y,
                         (info->range1 != 0U) ? "S" : "W",
                         FONT_SMALL, VIEW_COLOR_CH1, VIEW_COLOR_BAR_BG);
    (void)Lcd_DrawString((uint16_t)(STATUS_X_MODE + (2U * FONT_SMALL_WIDTH)),
                         STATUS_TEXT_Y, (info->range2 != 0U) ? "S" : "W",
                         FONT_SMALL, VIEW_COLOR_CH2, VIEW_COLOR_BAR_BG);

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
