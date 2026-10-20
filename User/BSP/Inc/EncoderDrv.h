/**
  * @file    EncoderDrv.h
  * @brief   双编码器增量读取
  *
  * 为什么不直接用 Encoder_Get()
  * ---------------------------
  * Encoder_Get() 只读 TIM8 的累计计数值，既无法区分两个编码器，
  * 也不返回"这次转了多少"。UI 需要的是增量，所以这里额外封装一份：
  * 每次读取时返回与上次的差值，并把差值吃掉。
  */

#ifndef __ENCODER_DRV_H
#define __ENCODER_DRV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENCODER_DRV_1       1U   /* TIM8，PC6/PC7 */
#define ENCODER_DRV_2       2U   /* TIM4，PB6/PB7 */

/**
  * @brief  以当前位置作为增量基准（初始化和切页时调用）
  */
void EncoderDrv_Reset(void);

/**
  * @brief  读取自上次调用以来的增量
  * @param  id ENCODER_DRV_1 或 ENCODER_DRV_2
  * @retval 有符号增量，单位是"格"：转一格 = ±1
  * @note   硬件配置未改（定时器仍是 x4 编码器模式，一格产生 4 个计数），
  *         折算与余数累加都在本函数内部完成。
  */
int16_t EncoderDrv_ReadDelta(uint8_t id);

#ifdef __cplusplus
}
#endif

#endif /* __ENCODER_DRV_H */
