/**
  * @file    EncoderDrv.c
  * @brief   双编码器增量读取实现
  *
  * 两个编码器都已由 CubeMX 生成的 tim.c 启动：
  *   TIM8 -> 编码器1，PC6/PC7
  *   TIM4 -> 编码器2，PB6/PB7
  * 这里只做"读计数 -> 求差 -> 记住新值"，不改变计数方向或分频。
  */

#include "EncoderDrv.h"

#include "tim.h"

/*
 * 硬件上是 x4 计数（TIM8/TIM4 都配成 Encoder Mode TI1 and TI2）：
 * 编码器转一格 = 一个完整正交周期 = 4 个计数。
 * 上层要的是"转一格加 1"，所以这里把原始计数除以 4。
 *
 * 为什么不能直接 delta / 4：
 * 主循环一次要 100 ms 以上，转一格的过程中可能被拆成几次读到
 * （1+1+1+1），每次整数除 4 都得到 0，就会丢步。
 * 因此把余数累加，攒够 4 个计数才吐一格。
 */
#define ENCODER_DRV_COUNTS_PER_DETENT   4

static int16_t  s_last1;
static int16_t  s_last2;
static int32_t  s_rem1;
static int32_t  s_rem2;
static uint8_t  s_ready;

static int16_t EncoderDrv_ReadRaw(uint8_t id)
{
    if (id == ENCODER_DRV_2)
    {
        return (int16_t)__HAL_TIM_GET_COUNTER(&htim4);
    }
    return (int16_t)__HAL_TIM_GET_COUNTER(&htim8);
}

/* 把原始计数增量折算成"格"，余数留在 *rem 里等下次 */
static int16_t EncoderDrv_ToDetents(int16_t raw_delta, int32_t *rem)
{
    int32_t steps;

    *rem += (int32_t)raw_delta;
    steps = *rem / ENCODER_DRV_COUNTS_PER_DETENT;
    *rem -= steps * ENCODER_DRV_COUNTS_PER_DETENT;

    return (int16_t)steps;
}

void EncoderDrv_Reset(void)
{
    s_last1 = EncoderDrv_ReadRaw(ENCODER_DRV_1);
    s_last2 = EncoderDrv_ReadRaw(ENCODER_DRV_2);
    s_rem1  = 0;
    s_rem2  = 0;
    s_ready = 1U;
}

int16_t EncoderDrv_ReadDelta(uint8_t id)
{
    int16_t now;
    int16_t delta;

    if (s_ready == 0U)
    {
        EncoderDrv_Reset();
        return 0;
    }

    now = EncoderDrv_ReadRaw(id);

    if (id == ENCODER_DRV_2)
    {
        delta   = (int16_t)(now - s_last2);
        s_last2 = now;
        return EncoderDrv_ToDetents(delta, &s_rem2);
    }

    delta   = (int16_t)(now - s_last1);
    s_last1 = now;
    return EncoderDrv_ToDetents(delta, &s_rem1);
}
