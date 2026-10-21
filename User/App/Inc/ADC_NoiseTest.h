/**
  * @file    ADC_NoiseTest.h
  * @brief   ADC 噪声 A/B 对比（临时诊断，串口打印，不启动 RTOS）
  *
  * 目的：把"波形噪声大"拆成两个可测量的部分，而不是靠猜。
  *       两组条件跑的是同一条采集链路（ADC 双同步 + DMA + TIM2 触发）：
  *
  *   - A 组：采集时不刷屏  —— 等价于裸机版本（先把 320 点采完，再画）
  *   - B 组：采集期间整屏重画 —— 等价于 RTOS 版本（采集与刷屏并行发生）
  *
  * 两组各自统计一帧内的码值跨度（spread = max - min），
  * B 明显大于 A 就说明 LCD 的 SPI 突发确实耦合进了采样窗口；
  * 两组一样大则说明噪声来自前端电路本身（偏置网络、地线、供电）。
  *
  * 用法：把 Core/Src/main.c 的 APP_RUN_NOISETEST 置 1，烧录后看串口。
  */
#ifndef __ADC_NOISETEST_H
#define __ADC_NOISETEST_H

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  跑一轮 A/B 对比并把统计结果打到 USART1
  * @note   裸机阻塞实现，每轮之间停顿约 2 s；只能在没有任务运行时调用。
  */
void ADC_NoiseTest_Run(void);

#ifdef __cplusplus
}
#endif

#endif /* __ADC_NOISETEST_H */
