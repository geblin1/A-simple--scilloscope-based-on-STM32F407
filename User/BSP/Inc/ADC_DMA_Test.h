/*
 * ADC1/ADC2 双 ADC 规则同步 + DMA 采集测试接口
 *
 * 实现见 User/BSP/Src/ADC_DMA_Test.c
 */
#ifndef __ADC_DMA_TEST_H
#define __ADC_DMA_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

uint8_t ADC_DMA_Test_Once(uint32_t *buf, uint32_t len);
void    ADC_DMA_Test_Run(void);

#ifdef __cplusplus
}
#endif

#endif /* __ADC_DMA_TEST_H */
