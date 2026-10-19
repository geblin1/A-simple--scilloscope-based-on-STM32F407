/*
 * @Date: 2026-07-02 20:00:33
 * @LastEditTime: 2026-10-19 19:56:56
 * @FilePath: \01_基础驱动代码\User\BSP\Src\Encoder.c
 * @Description: 编码器驱动代码
 */
#include "tim.h"
int16_t Encoder_Get(void)
{
	/*使用Temp变量作为中继，目的是返回CNT后将其清零*/
	int16_t Temp;
	Temp = __HAL_TIM_GET_COUNTER(&htim8);
	// __HAL_TIM_SET_COUNTER(&htim8, 0);
	return Temp;
}
