/**
  * @file    ScopeMenu.h
  * @brief   设置菜单的绘制（覆盖在波形区上方的小弹窗）
  *
  * 菜单只负责"画"，选中行、各项当前值都由 App 层持有并传入。
  */

#ifndef __SCOPE_MENU_H
#define __SCOPE_MENU_H

#include <stdint.h>

#include "Trigger.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCOPE_MENU_ROWS           7U
#define SCOPE_MENU_ROW_VDIV       0U
#define SCOPE_MENU_ROW_TIM        1U
#define SCOPE_MENU_ROW_TRIG_SRC   2U
#define SCOPE_MENU_ROW_TRIG_EDGE  3U
#define SCOPE_MENU_ROW_TRIG_LEVEL 4U
#define SCOPE_MENU_ROW_AUTO       5U   /* AUTO：KEY4 自动选 V/div 与 Time/div */
#define SCOPE_MENU_ROW_CAL        6U   /* 前端标定：KEY1 选通道、KEY4 开始 */

typedef struct
{
    uint8_t      sel;          /* 当前选中行 0~6 */
    uint16_t     vdiv_mv1;     /* CH1 的垂直档位 */
    uint16_t     vdiv_mv2;     /* CH2 的垂直档位（两路可以不同） */
    uint32_t     tdiv_us;      /* Time/div */
    uint8_t      trig_src;     /* 触发通道：1 = CH1，2 = CH2 */
    Trigger_Edge trig_edge;    /* 触发边沿：关 / 上升 / 下降 */
    int32_t      trig_level_mv;/* 触发电平（mV，带符号） */
    uint8_t      measure_ch;   /* 当前通道：1 = CH1，2 = CH2（V/div、Cal 行按它） */
} ScopeMenu_Info;

/**
  * @brief  在波形区中央画出设置菜单
  * @param  info 当前选中行与各项数值
  */
void ScopeMenu_Draw(const ScopeMenu_Info *info);

#ifdef __cplusplus
}
#endif

#endif /* __SCOPE_MENU_H */
