/**
  * @file    ScopeView.h
  * @brief   视图层：屏幕布局、配色与三个绘制模块的接口
  *
  * 分层说明
  * --------
  *   App 层   —— 采集、状态机、按键/编码器处理，决定"显示什么"
  *   View 层  —— 本文件。只负责"怎么画"，不采集、不读按键、不持有全局状态
  *   BSP 层   —— Lcd / Font 提供像素与字符
  *
  * 三个横条
  * --------
  *   y   0 ~  21  状态栏  22 px  通道、V/div、Time/div、耦合、运行状态
  *   y  22 ~ 211  波形区 190 px  8 x 10 网格，每格 40 x 19 px
  *   y 212 ~ 239  参数栏  28 px  频率、周期、Vpp、RMS、直流分量
  *
  * 所有坐标均为横屏 320x240 下的绝对坐标。
  */

#ifndef __SCOPE_VIEW_H
#define __SCOPE_VIEW_H

#include <stdint.h>

#include "Font.h"
#include "Lcd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* 布局                                                                */
/* ================================================================== */
#define VIEW_W              ((uint16_t)LCD_WIDTH)    /* 320 */
#define VIEW_H              ((uint16_t)LCD_HEIGHT)   /* 240 */

#define VIEW_STATUS_Y       0U
#define VIEW_STATUS_H       22U

#define VIEW_WAVE_Y         22U
#define VIEW_WAVE_H         190U

#define VIEW_PARAM_Y        212U
#define VIEW_PARAM_H        28U

/* 波形区网格：8 列 x 10 行 */
#define VIEW_GRID_COLS      8U
#define VIEW_GRID_ROWS      10U
#define VIEW_CELL_W         (VIEW_W / VIEW_GRID_COLS)     /* 40 px */
#define VIEW_CELL_H         (VIEW_WAVE_H / VIEW_GRID_ROWS) /* 19 px */

/* 波形区中心线（0 V 所在的 y） */
#define VIEW_WAVE_MID_Y     ((int16_t)(VIEW_WAVE_Y + (VIEW_WAVE_H / 2U)))

/* 垂直方向每格像素数（V/div 换算用这个） */
#define VIEW_PX_PER_VDIV    ((int32_t)VIEW_CELL_H)

/* ================================================================== */
/* 配色（RGB565）                                                      */
/* ================================================================== */
#define VIEW_COLOR_BG       0x0841U   /* 近黑背景 */
#define VIEW_COLOR_GRID     0x2124U   /* 网格线   */
#define VIEW_COLOR_AXIS     0x4208U   /* 中心轴   */
#define VIEW_COLOR_TEXT     0xFFFFU   /* 正文白   */
#define VIEW_COLOR_LABEL    0x8410U   /* 灰标签   */
#define VIEW_COLOR_CH1      0xFFE0U   /* 通道1：配置为黄，实际面板显示偏蓝 */
#define VIEW_COLOR_CH2      0x07FFU   /* 通道2：配置为青，实际面板显示偏红 */
#define VIEW_COLOR_RUN      0x07E0U   /* 运行 绿  */
#define VIEW_COLOR_HOLD     0xFD20U   /* 停止 琥珀 */
#define VIEW_COLOR_SELECT   0xF800U   /* 选中 红  */
#define VIEW_COLOR_BAR_BG   0x0000U   /* 条带背景 */

/* ================================================================== */
/* 视图数据                                                            */
/* ================================================================== */

/**
  * @brief  状态栏要显示的信息
  * @note   全部是"已经算好的显示量"，View 不再做单位换算之外的逻辑
  */
typedef struct
{
    uint8_t  channel;       /* 当前选中通道：1 / 2 */
    uint16_t vdiv_mv;       /* 当前通道 V/div，单位 mV（100~10000） */
    uint32_t tdiv_us;       /* Time/div，单位 us */
    uint32_t sample_rate;   /* 实际采样率，单位 Sa/s */
    uint8_t  ac_dc;         /* 0 = DC，1 = AC */
    uint8_t  range;         /* 前端量程：0 = 大量程(W)，1 = 小量程(S) */
    uint8_t  running;       /* 1 = RUN，0 = HOLD */
    uint8_t  rate_limited;  /* 1 = 采样率被硬件上限钳位，显示 LIMIT */
} StatusBar_Info;

/**
  * @brief  参数栏要显示的信息
  * @note   数值用整数定点表示，避免浮点运算与 %f（本工程未开 printf 浮点）
  */
typedef struct
{
    uint8_t  valid;         /* 0 = 数据不足，画 "--" */
    uint8_t  channel;       /* 测量对应的通道 */
    int32_t  freq_mhz;      /* 频率，单位 0.001 Hz */
    int32_t  period_us;     /* 周期，单位 us */
    int32_t  vpp_mv;        /* 峰峰值 mV */
    int32_t  rms_mv;        /* 有效值 mV */
    int32_t  mean_mv;       /* 直流分量 mV */
} ParamBar_Info;

/**
  * @brief  波形绘制所需的数据
  * @note   样本已经是"探头看到的输入电压（mV，带符号）"，由 App 层用
  *         Frontend_RawToMv() 换算好再传进来；View 不做任何标定，只按
  *         V/div 把 mV 映射成像素。负电压自然落在中轴线以下。
  */
typedef struct
{
    const int32_t *ch1;     /* 通道1 样本（mV），可为 0 */
    const int32_t *ch2;     /* 通道2 样本（mV），可为 0 */
    uint16_t        count;  /* 每通道样本数，最多 320 */
    uint16_t        vdiv_mv1;
    uint16_t        vdiv_mv2;
    uint8_t         show1;
    uint8_t         show2;
} WaveView_Data;

/* ================================================================== */
/* 绘制接口                                                            */
/* ================================================================== */

/**
  * @brief  清屏并画状态栏/参数栏的底板与波形区网格
  * @note   只在进入主界面、切换页面或需要整屏重画时调用一次
  */
void View_DrawFrame(void);

/**
  * @brief  重画状态栏（22 px 高）
  */
void StatusBar_Draw(const StatusBar_Info *info);

/**
  * @brief  重画参数栏（28 px 高）
  */
void ParamBar_Draw(const ParamBar_Info *info);

/**
  * @brief  只重画波形区背景（网格），用于清除旧波形
  */
void WaveView_DrawGrid(void);

/**
  * @brief  作废"屏幕上已有曲线"的假设
  * @note   波形区被别的东西盖过（弹出菜单、提示文字等）之后调用，
  *         下一次 WaveView_DrawTrace() 会整片重画而不是只补脏行。
  */
void WaveView_Invalidate(void);

/**
  * @brief  在网格上画两个通道的波形（不清背景）
  * @note   ch1/ch2 传 0 表示不画该通道
  */
void WaveView_DrawTrace(const WaveView_Data *data);

/**
  * @brief  在波形区正中显示一条提示文字（如 "WAIT..." / "NO SIGNAL"）
  * @param  text 以 '\0' 结尾的字符串
  */
void WaveView_DrawMessage(const char *text);

/**
  * @brief  在波形区左上角画一行小字（用于 HOLD/触发等角标）
  */
void WaveView_DrawBadge(const char *text, uint16_t fg);

#ifdef __cplusplus
}
#endif

#endif /* __SCOPE_VIEW_H */
