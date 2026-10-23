/**
  * @file    Fft.h
  * @brief   256 点实数 FFT（定点整数，纯 Algorithm 层，无 HAL / 无浮点）
  *
  * 用途
  * ----
  *   取一屏 320 点时域样本里居中的 256 点做 FFT，输出 128 条谱线
  *   （0 .. 采样率/2）的幅度，供界面画频谱。
  *
  * 定点约定
  * --------
  *   输入：mV（int32，带符号）。App 层已经用 Frontend 换算过，View/Algorithm
  *         不关心量程和标定。
  *   窗函数：Hann，Q15；旋转因子与 log2 表都由 tools/gen_fft_tables.py
  *         生成（FftTables.h），目标板不需要链接 libm / 软浮点。
  *   输出：mag_db[k] = 10*log10(P[k] / Pmax)（dB，0 = 最强谱线），
  *         范围 [-60, 0]。bin0（直流）固定 -60、不参与最强谱线选择：
  *         前端偏置和信号的直流分量会把交流谱压平，示波器 FFT
  *         通常只看交流分量。
  *
  * 分辨率与显示
  * ------------
  *   谱线间隔 = sample_rate / 256（40 kSa/s 时 156.25 Hz）。
  *   界面上 8 格对应 0 .. sample_rate/2，即每格 = sample_rate/16。
  */

#ifndef __FFT_H
#define __FFT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FFT_SIZE        256U
#define FFT_BINS        (FFT_SIZE / 2U)   /* 128 条谱线（0..127） */

/**
  * @brief  FFT 结果（整数定点）
  */
typedef struct
{
    uint16_t bins;                  /* 有效谱线数 = FFT_BINS */
    int16_t  mag_db[FFT_BINS];      /* 幅度谱，0 = 最强谱线，-60 为底 */
    uint8_t  peak_bin;              /* 最强谱线下标（1 .. FFT_BINS-1） */
} Fft_Result;

/**
  * @brief  对一段时域样本做 256 点 FFT
  * @param  samples  已标定的输入电压（mV，带符号）
  * @param  count    样本个数；取居中的 256 点，小于 256 时直接返回
  * @param  out      结果（bins / mag_db / peak_bin）
  * @note   内部使用约 2 KB 静态缓冲，不可重入；只在界面任务里调用。
  */
void Fft_Analyze(const int32_t *samples, uint16_t count, Fft_Result *out);

#ifdef __cplusplus
}
#endif

#endif /* __FFT_H */
