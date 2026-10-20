/**
  * @file    Frontend.h
  * @brief   模拟前端标定：把 ADC 原始码值换算成"输入电压（mV）"
  *
  * 为什么需要这一层
  * ----------------
  * 改造后的前端在 ADC 引脚上叠加了 1.65 V 偏置，并且对输入做了衰减：
  *
  *   大量程（跳线乙不插）：零点 1571 mV，增益 1/21    ，可测 −33.0 … +36.3 V
  *   小量程（跳线乙插 MID）：零点 1065 mV，增益 1/2.818，可测 −3.00 … +6.30 V
  *
  * 所以"码值 0"不再是"输入 0 V"，必须减零点、乘增益倒数才能得到真实输入电压。
  * View 层与 Algorithm 层都只认识"输入 mV"，换算全部收敛到本模块，
  * 将来换电阻、换偏置点只需要改这里的标定表。
  *
  * 跳线位置软件无法检测（没有检测引脚），所以量程由 App 层根据 V/div 档位选定，
  * 界面用 S / W 提示用户当前应该把跳线插在哪一档（见 Frontend_JumperHint）。
  *
  * 数值来源：FRONTEND_BIAS_DESIGN.md 第七节；CH1 实测校准
  *           （大量程 GND 实测 raw 1941 / 模型 1950，小量程 1320 / 模型 1321）。
  */

#ifndef __FRONTEND_H
#define __FRONTEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 满量程与参考电压（12 bit ADC） */
#define FRONTEND_RAW_FULL   4095L
#define FRONTEND_VREF_MV    3300L

/**
  * @brief  前端的两种量程
  * @note   与跳线乙位置一一对应，但软件不能检测，只能由用户保持同步
  */
typedef enum
{
    FRONTEND_RANGE_WIDE  = 0,   /* 大量程：跳线乙不插 */
    FRONTEND_RANGE_SMALL = 1    /* 小量程：跳线乙插 MID */
} Frontend_Range;

/* ---- 量程选择 ---- */

/** 设置当前使用的量程（只影响换算，不碰硬件） */
void Frontend_SetRange(Frontend_Range range);

/** 读回当前量程 */
Frontend_Range Frontend_GetRange(void);

/** 量程单字母标识："W" / "S"，状态栏用 */
const char *Frontend_RangeLetter(void);

/** 量程名称："WIDE" / "SMALL"，菜单用 */
const char *Frontend_RangeName(void);

/** 当前量程要求的跳线乙位置："OPEN"（不插）/ "MID" */
const char *Frontend_JumperHint(void);

/* ---- 换算 ---- */

/**
  * @brief  原始码值 -> 输入电压
  * @param  raw ADC 原始码值（0~4095）
  * @return 输入电压，单位 mV，**带符号**（负电压为负值）
  * @note   已扣除偏置零点并补偿衰减，可直接当"探头看到的电压"使用
  */
int32_t Frontend_RawToMv(uint16_t raw);

/** 输入 0 V 对应的理论码值（大量程 1950 / 小量程 1321） */
int32_t Frontend_ZeroRaw(void);

/** 输入 0 V 时 ADC 引脚上的电压（大量程 1571 mV / 小量程 1065 mV） */
int32_t Frontend_ZeroMv(void);

/** 当前量程可测的输入下限（mV，负值） */
int32_t Frontend_MinMv(void);

/** 当前量程可测的输入上限（mV，正值） */
int32_t Frontend_MaxMv(void);

/** 当前量程下每 1 个码值对应多少 0.001 mV（即分辨率，用于显示） */
int32_t Frontend_MvPerLsbMilli(void);

#ifdef __cplusplus
}
#endif

#endif /* __FRONTEND_H */
