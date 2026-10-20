/**
  * @file    Measure.h
  * @brief   波形测量算法：Vpp / RMS / 直流分量 / 频率 / 周期
  *
  * 纯算法层：只吃"输入电压数组（mV，带符号）+ 采样率"，
 * 不碰任何外设、全局状态或屏幕，也不知道 ADC / 偏置的存在。
  */

#ifndef __MEASURE_H
#define __MEASURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  一次测量的结果
  * @note   电压单位 mV（可带符号），频率单位 0.001 Hz，周期单位 us
  */
typedef struct
{
    uint8_t  valid;       /* 1 = 数据可用 */
    int32_t  vpp_mv;      /* 峰峰值 */
    int32_t  rms_mv;      /* 有效值（含直流分量） */
    int32_t  mean_mv;     /* 直流分量 */
    int32_t  freq_mhz;    /* 频率，0.001 Hz 为单位；0 表示测不到 */
    int32_t  period_us;   /* 周期，us；0 表示测不到 */
} Measure_Result;

/**
  * @brief  分析一段波形
  * @param  samples      输入电压数组，单位 mV，**带符号**（已由 Frontend 扣零点、补增益）
  * @param  count        样本个数
  * @param  sample_rate  采样率 Sa/s，用于把样本间隔换算成时间
  * @param  out          输出结果（不会被置 0 之外的内容写入）
  * @note   本模块只认"输入 mV"，不关心 ADC 码值、偏置或量程，
  *         换算全部由 User/BSP/Src/Frontend.c 负责。
  */
void Measure_Analyze(const int32_t *samples, uint16_t count,
                     uint32_t sample_rate, Measure_Result *out);

#ifdef __cplusplus
}
#endif

#endif /* __MEASURE_H */
