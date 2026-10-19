/*
 * ADC1 / ADC2 自检（纯测试代码，整个文件夹里的两个文件删掉即可移除）
 *
 * 工程当前 ADC 配置（CubeMX 生成，见 Core/Src/adc.c）：
 *   ADC1 = PA0 (ADC_CHANNEL_0)   ADC2 = PA1 (ADC_CHANNEL_1)
 *   双 ADC 规则同步 ADC_DUALMODE_REGSIMULT + DMA 模式 2
 *   12 位、右对齐、采样时间 15 周期、单次转换、软件触发
 */
#ifndef __ADC_TEST_H
#define __ADC_TEST_H

#include <stdint.h>

/*
 * 采一次（ADC1 与 ADC2 同时转换），结果分别写回 *adc1 / *adc2。
 * 返回 1 成功，0 失败（HAL 启动或超时）。
 */
uint8_t ADC_Test_Sample(uint32_t *adc1, uint32_t *adc2);

/*
 * 连采 ADC_TEST_SAMPLES(8) 次，把平均值、电压、波动范围和结论用一行 ASCII
 * 打印到 USART1。阻塞执行，耗时约几十微秒，可放进主循环配合 HAL_Delay 连续观察。
 * 第一次调用会额外打印一行引脚说明。
 */
void ADC_Test_Run(void);

#endif /* __ADC_TEST_H */
