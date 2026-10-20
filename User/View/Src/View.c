/**
  * @file    View.c
  * @brief   视图层总入口：整屏清屏与框架绘制
  *
  * 为什么单独一个文件
  * ------------------
  * StatusBar / ParamBar / WaveView 各自只负责一条区域；"整屏长什么样"
  * 这件事需要一个统一的地方，否则 App 层就要知道三块区域怎么摆。
  * 这里只提供 View_DrawFrame()，App 只需调用它一次。
  */

#include "ScopeView.h"

void View_DrawFrame(void)
{
    /* 整屏铺底 */
    Lcd_Fill(VIEW_COLOR_BG);

    /* 波形区网格（同时把波形区背景重画一遍） */
    WaveView_DrawGrid();

    /* 上下的条带底板：具体内容由 App 调用 StatusBar_Draw / ParamBar_Draw 填充 */
    Lcd_FillRect(0U, VIEW_STATUS_Y, VIEW_W, VIEW_STATUS_H, VIEW_COLOR_BAR_BG);
    Lcd_FillRect(0U, VIEW_PARAM_Y, VIEW_W, VIEW_PARAM_H, VIEW_COLOR_BAR_BG);
}
