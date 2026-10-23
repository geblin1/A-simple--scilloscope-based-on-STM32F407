/**
  * @file    Frontend.h
  * @brief   模拟前端标定：把 ADC 原始码值换算成"输入电压（mV）"
  *
  * 为什么需要这一层
  * ----------------
  * 改造后的前端在 ADC 引脚上叠加了 1.65 V 偏置，并且对输入做了衰减：
  *
  *   大量程（跳线乙不插）：零点约 1571 mV，增益约 1/21   ，可测 −33.0 … +36.3 V
  *   小量程（跳线乙插 MID）：零点约 1065 mV，增益约 1/2.82，可测 −3.00 … +6.30 V
  *
  * 所以"码值 0"不再是"输入 0 V"，必须减零点、乘增益倒数才能得到真实输入电压。
  * View 层与 Algorithm 层都只认识"输入 mV"，换算全部收敛到本模块。
  *
  * 每个通道各有一套标定
  * --------------------
  * CH1 / CH2 是两条独立的 ADC 通路，零点与增益都有器件差异；每个通道、
  * 每个量程各存一套（共 4 套）。标定量由 App 菜单的 Cal 行在设备上实测：
  * KEY1 选通道、KEY4 启动两点法——接 GND 采零点码值、接 3.3 V 采满点码值，
  * 由两点反推零点与增益。3.3 V 同时是 ADC 基准，没有万用表时就是最方便的
  * 真值（见 App_Scope.c 的标定流程）。
  *
  * 结果保存到 flash 最后一个扇区（0x080E0000），上电由 Frontend_Init()
  * 读回；没有有效数据时退回设计默认值。
  *
  * 跳线位置软件无法检测（没有检测引脚），所以每个通道的量程分别由 App 层
  * 根据该通道的 V/div 档位选定，界面用 S / W 分别提示两路应该把跳线插在哪一档
  * （见 Frontend_JumperHint）。标定时会校验实测码值差是否落在当前量程的预期
  * 区间内，防止把错配采到的数据固化成标定值。
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

/* 通道编号：与 App 层 measure_ch / trig_src 一致 */
#define FRONTEND_CH1        1U
#define FRONTEND_CH2        2U

/**
  * @brief  前端的两种量程
  * @note   与跳线乙位置一一对应，但软件不能检测，只能由用户保持同步
  */
typedef enum
{
    FRONTEND_RANGE_WIDE  = 0,   /* 大量程：跳线乙不插 */
    FRONTEND_RANGE_SMALL = 1    /* 小量程：跳线乙插 MID */
} Frontend_Range;

/* ---- 初始化与标定 ---- */

/**
  * @brief  上电初始化：从 flash 读回各通道标定，无有效数据时用设计默认值
  * @note   必须在第一次调用 Frontend_RawToMv() 之前调用一次
  */
void Frontend_Init(void);

/**
  * @brief  两点标定（只作用于当前量程，通道由参数指定）
  * @param  channel  1 = CH1，2 = CH2
  * @param  zero_raw 输入接 GND 时采到的平均码值
  * @param  span_raw 输入接 3.3 V 时采到的平均码值（按 3300 mV 处理）
  * @return 1 = 参数合理并已更新到 RAM；0 = 不合理，标定保持不变
  * @note   成功后再调 Frontend_CalSave() 写入 flash；码值差超出当前量程的
  *         预期区间时返回 0（多半是跳线位置和 V/div 档位对不上）
  */
uint8_t Frontend_Calibrate(uint8_t channel, uint16_t zero_raw, uint16_t span_raw);

/**
  * @brief  把四套标定写入 flash（掉电保存）
  * @return 1 = 成功；0 = 擦除/编程失败（RAM 里的标定仍然有效）
  */
uint8_t Frontend_CalSave(void);

/* ---- 量程选择（逐通道：两路前端可以处在不同量程） ---- */

/** 设置某通道的当前量程（只影响换算，不碰硬件） */
void Frontend_SetRange(uint8_t channel, Frontend_Range range);

/** 读回某通道的当前量程 */
Frontend_Range Frontend_GetRange(uint8_t channel);

/** 某通道的量程单字母标识："W" / "S"，状态栏用 */
const char *Frontend_RangeLetter(uint8_t channel);

/** 某通道的量程名称："WIDE" / "SMALL"，菜单用 */
const char *Frontend_RangeName(uint8_t channel);

/** 某通道当前量程要求的跳线乙位置："OPEN"（不插）/ "MID" */
const char *Frontend_JumperHint(uint8_t channel);

/* ---- 换算 ---- */

/**
  * @brief  原始码值 -> 输入电压
  * @param  channel 1 = CH1，2 = CH2（未知值按 CH1 处理）
  * @param  raw ADC 原始码值（0~4095）
  * @return 输入电压，单位 mV，**带符号**（负电压为负值）
  * @note   已扣除各通道实测零点并补偿衰减，可直接当"探头看到的电压"使用
  */
int32_t Frontend_RawToMv(uint8_t channel, uint16_t raw);

/** 某通道当前量程下输入 0 V 对应的码值 */
int32_t Frontend_ZeroRaw(uint8_t channel);

/** 某通道当前量程下输入 0 V 时 ADC 引脚上的电压（mV） */
int32_t Frontend_ZeroMv(uint8_t channel);

/** 某通道当前量程可测的输入下限（mV，负值） */
int32_t Frontend_MinMv(uint8_t channel);

/** 某通道当前量程可测的输入上限（mV，正值） */
int32_t Frontend_MaxMv(uint8_t channel);

/** 某通道当前量程下每 1 个码值对应多少 0.001 mV（即分辨率，用于显示） */
int32_t Frontend_MvPerLsbMilli(uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif /* __FRONTEND_H */
