/**
  * @file    ScopeMenu.h
  * @brief   设置菜单的绘制（覆盖在波形区上方的小弹窗）
  *
  * 菜单只负责"画"，选中行、各项当前值都由 App 层持有并传入。
  */

#ifndef __SCOPE_MENU_H
#define __SCOPE_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCOPE_MENU_ROWS     2U
#define SCOPE_MENU_ROW_VDIV 0U
#define SCOPE_MENU_ROW_TIM  1U

typedef struct
{
    uint8_t  sel;        /* 当前选中行 0~1 */
    uint16_t vdiv_mv;    /* 垂直档位（双通道共用） */
    uint32_t tdiv_us;    /* Time/div */
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
