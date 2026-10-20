/**
  * @file    ScopeMenu.c
  * @brief   设置菜单绘制实现
  *
  * 弹窗尺寸：220 x 78，居中覆盖在波形区。双通道共用同一个垂直档位，
  * 所以菜单里只有"垂直"和"水平"两项：
  *   +--------------------------------------+
  *   | SETTINGS                             |
  *   | V/div                500mV           |  <- 选中行红底
  *   | Time/div             1.00ms          |
  *   +--------------------------------------+
  */

#include "ScopeMenu.h"

#include <stdio.h>

#include "ScopeView.h"

#define MENU_W          220U
#define MENU_H          78U
#define MENU_X          ((uint16_t)((VIEW_W - MENU_W) / 2U))
#define MENU_Y          92U
#define MENU_PAD        6U
#define MENU_ROW_H      20U
#define MENU_TITLE_Y    ((uint16_t)(MENU_Y + 4U))

/* 行文字基线：第一行顶部 = MENU_Y + 24 */
#define MENU_ROW_Y(i)   ((uint16_t)(MENU_Y + 24U + ((i) * MENU_ROW_H)))

/* "500mV" / "5.00V" */
static void Menu_FormatVdiv(uint16_t mv, char *buf, size_t n)
{
    if (mv >= 1000U)
    {
        (void)snprintf(buf, n, "%u.%02uV", (unsigned)(mv / 1000U),
                       (unsigned)((mv % 1000U) / 10U));
    }
    else
    {
        (void)snprintf(buf, n, "%umV", (unsigned)mv);
    }
}

/* "1.00ms" / "20us" */
static void Menu_FormatTdiv(uint32_t us, char *buf, size_t n)
{
    if (us >= 1000U)
    {
        (void)snprintf(buf, n, "%lu.%02lums", (unsigned long)(us / 1000U),
                       (unsigned long)((us % 1000U) / 10U));
    }
    else
    {
        (void)snprintf(buf, n, "%luus", (unsigned long)us);
    }
}

void ScopeMenu_Draw(const ScopeMenu_Info *info)
{
    char     text[20];
    char     value[16];
    uint16_t i;
    uint16_t y;

    if (info == 0)
    {
        return;
    }

    /* 弹窗底板与边框 */
    Lcd_FillRect(MENU_X, MENU_Y, MENU_W, MENU_H, VIEW_COLOR_BAR_BG);
    Lcd_DrawRect(MENU_X, MENU_Y, MENU_W, MENU_H, VIEW_COLOR_SELECT);

    /* 标题 */
    (void)Lcd_DrawString((uint16_t)(MENU_X + MENU_PAD), MENU_TITLE_Y, "SETTINGS",
                         FONT_SMALL, VIEW_COLOR_SELECT, VIEW_COLOR_BAR_BG);

    for (i = 0U; i < SCOPE_MENU_ROWS; i++)
    {
        uint16_t bg = (i == info->sel) ? VIEW_COLOR_SELECT : VIEW_COLOR_BAR_BG;
        uint16_t fg = (i == info->sel) ? VIEW_COLOR_BAR_BG : VIEW_COLOR_TEXT;

        y = MENU_ROW_Y(i);

        switch (i)
        {
            case SCOPE_MENU_ROW_VDIV:
                (void)snprintf(text, sizeof(text), "V/div");
                Menu_FormatVdiv(info->vdiv_mv, value, sizeof(value));
                break;
            default:
                (void)snprintf(text, sizeof(text), "Time/div");
                Menu_FormatTdiv(info->tdiv_us, value, sizeof(value));
                break;
        }

        /* 整行底色（选中时红底） */
        Lcd_FillRect((uint16_t)(MENU_X + 2U), (uint16_t)(y - 5U),
                     (uint16_t)(MENU_W - 4U), FONT_SMALL_HEIGHT + 2U, bg);

        (void)Lcd_DrawString((uint16_t)(MENU_X + MENU_PAD), y, text, FONT_SMALL, fg, bg);
        (void)Lcd_DrawString((uint16_t)(MENU_X + MENU_W - MENU_PAD -
                                        Lcd_TextWidth(FONT_SMALL, value)),
                             y, value, FONT_SMALL, fg, bg);
    }

    /* 底部提示 */
    (void)Lcd_DrawString((uint16_t)(MENU_X + MENU_PAD),
                         (uint16_t)(MENU_Y + MENU_H - FONT_SMALL_HEIGHT - 4U),
                         "KEY2: back   ROT: change", FONT_SMALL,
                         VIEW_COLOR_LABEL, VIEW_COLOR_BAR_BG);
}
