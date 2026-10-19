/**
  * @file    ADC_Drv.h
  * @brief   双 ADC（ADC1=PA0 / ADC2=PA1）同步采集驱动 —— 对外只暴露 API
  *
  * 设计目标：
  *   1. 业务代码只调用本文件的函数，不再出现 HAL 调用、寄存器操作、DMA 细节；
  *   2. 与 FreeRTOS 兼容：中断里只用 ...FromISR 接口，等待用信号量，
  *      DMA 中断优先级 = configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY，可安全调用；
  *      调度器没启动时自动退化成轮询 + HAL_Delay，同一份代码两种运行方式都能用；
  *   3. 所有阻塞接口都带超时，不会死等。
  *
  * 硬件约定（由 CubeMX 配置好，本驱动不再改）：
  *   - ADC1 = 主器件，规则通道 0 = PA0；ADC2 = 从器件，规则通道 1 = PA1
  *   - 双 ADC 规则同步 + DMA 模式 2：一次搬运的 32 位字里，低 16 位是 ADC1、高 16 位是 ADC2
  *   - 触发源 = TIM2 的 TRGO(UPDATE)，TIM2 计数频率 1 MHz
  *   - 缓冲 = DMA2_Stream0，普通模式，一次采集一帧（len 个点）后停
  *
  * 使用约束：
  *   - 采集缓冲区必须放在 SRAM，**不能放 CCMRAM**（DMA 访问不到 CCMRAM）；
  *   - 本驱动独占 TIM2 和 DMA2_Stream0，别处不要再动这两个外设；
  *   - 本驱动实现了 HAL_ADC_ConvCpltCallback / HAL_ADC_ErrorCallback，
  *     业务代码不要再重复实现这两个回调；
  *   - 单帧长度上限 65535（DMA 的 NDTR 是 16 位的）。
  */
#ifndef __ADC_DRV_H
#define __ADC_DRV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 硬件常量 ---------------- */
#define ADC_DRV_FULL_SCALE        4095U        /* 12 位满量程 */
#define ADC_DRV_VREF_MV           3300U        /* 参考电压 mV */
#define ADC_DRV_RATE_MIN          1000U        /* 最低采样率 1 kSPS */
#define ADC_DRV_RATE_MAX          1000000U     /* 最高采样率 1 MSPS（3 cycles 采样时） */
#define ADC_DRV_CAPTURE_MAX       0xFFFFU      /* 单帧最大点数（NDTR 16 位） */

/* ---------------- 返回值 ---------------- */
typedef enum
{
    ADC_DRV_OK = 0,        /* 成功 */
    ADC_DRV_BUSY,          /* 正在采集，忙 */
    ADC_DRV_PARAM,         /* 参数非法 */
    ADC_DRV_TIMEOUT,       /* 等待超时 */
    ADC_DRV_ERROR          /* 硬件/状态错误 */
} ADC_Drv_Status;

/* ---------------- 初始化 ---------------- */
/**
  * @brief  初始化驱动（幂等，可重复调用）
  * @note   必须在 MX_ADC1_Init / MX_ADC2_Init 之后调用；
  *         在 vTaskStartScheduler() 之前或之后调用都可以。
  */
ADC_Drv_Status ADC_Drv_Init(void);

/* ---------------- 单次采样（适合慢速、偶发的读取） ---------------- */
/**
  * @brief  轮询方式读一次两路原始值（0~4095）
  * @param  ch1 输出 ADC1(PA0) 原始值
  * @param  ch2 输出 ADC2(PA1) 原始值
  * @note   内部临时退出多 ADC 模式、临时关闭硬件触发，结束后原样恢复；
  *         不要和 ADC_Drv_CaptureStart 同时使用（会返回 ADC_DRV_BUSY）。
  */
ADC_Drv_Status ADC_Drv_ReadOnce(uint16_t *ch1, uint16_t *ch2);

/**
  * @brief  轮询方式读一次两路电压值（mV）
  */
ADC_Drv_Status ADC_Drv_ReadOnceMv(uint16_t *mv1, uint16_t *mv2);

/* ---------------- 采样率（时基） ---------------- */
/**
  * @brief  设置采样率，对下一次 ADC_Drv_CaptureStart 生效
  * @param  sps    目标采样率，范围 [ADC_DRV_RATE_MIN, ADC_DRV_RATE_MAX]
  * @param  actual 输出实际能达到的采样率（可为 NULL）
  * @note   实际值是 1MHz/(ARR+1)，不一定等于目标值，以 actual 为准。
  */
ADC_Drv_Status ADC_Drv_SetSampleRate(uint32_t sps, uint32_t *actual);

/** @brief 当前配置的采样率（Hz） */
uint32_t ADC_Drv_GetSampleRate(void);

/** @brief 支持的采样率上下限 */
uint32_t ADC_Drv_GetMinSampleRate(void);
uint32_t ADC_Drv_GetMaxSampleRate(void);

/**
  * @brief  把“点数”换算成“时长（微秒）”，按当前采样率计算
  * @note   时基换算用，比如 1024 点 @200kSPS = 5120 us
  */
uint32_t ADC_Drv_SamplesToUs(uint32_t samples);

/* ---------------- 帧采集（定时器触发 + DMA，示波器主用） ---------------- */
/**
  * @brief  启动一帧采集，立即返回（非阻塞）
  * @param  buf 目标缓冲区，长度 >= len 个 uint32_t；**必须在 SRAM，不能是 CCMRAM**
  * @param  len 本帧点数，1 ~ ADC_DRV_CAPTURE_MAX
  * @note   每个元素低 16 位 = ADC1、高 16 位 = ADC2，可用
  *         ADC_Drv_WordCh1()/ADC_Drv_WordCh2() 取值；
  *         启动后由 TIM2 按设定采样率触发，采满自动停触发并通知。
  */
ADC_Drv_Status ADC_Drv_CaptureStart(uint32_t *buf, uint32_t len);

/**
  * @brief  等待本帧采集完成
  * @param  timeout_ms 超时（毫秒）
  * @note   FreeRTOS 调度器在跑时阻塞在信号量上（会让出 CPU）；
  *         没有调度器时轮询 + 延时。返回后采集资源已自动释放。
  *         超时后同样会释放资源，缓冲区内容为已采到的部分。
  */
ADC_Drv_Status ADC_Drv_CaptureWait(uint32_t timeout_ms);

/** @brief 本帧是否已完成（非阻塞查询，1=完成；完成不等于已释放资源） */
uint8_t ADC_Drv_CaptureIsDone(void);

/** @brief 本帧还剩多少个点没搬完（未在采集时返回 0） */
uint32_t ADC_Drv_CaptureRemaining(void);

/**
  * @brief  停止并释放采集资源（停定时器、停 DMA、清状态）
  * @note   任何时刻调用都安全，可重复调用；一般在
  *         ADC_Drv_CaptureIsDone() 返回 1 之后调用。
  */
ADC_Drv_Status ADC_Drv_CaptureStop(void);

/* ---------------- 数据换算 ---------------- */
/** @brief 从一个采集字里取出 ADC1(PA0) 的原始值 */
static inline uint16_t ADC_Drv_WordCh1(uint32_t word)
{
    return (uint16_t)(word & 0xFFFFU);
}

/** @brief 从一个采集字里取出 ADC2(PA1) 的原始值 */
static inline uint16_t ADC_Drv_WordCh2(uint32_t word)
{
    return (uint16_t)((word >> 16) & 0xFFFFU);
}

/** @brief 原始值转毫伏 */
static inline uint16_t ADC_Drv_ToMillivolt(uint16_t raw)
{
    return (uint16_t)(((uint32_t)raw * ADC_DRV_VREF_MV) / ADC_DRV_FULL_SCALE);
}

/* ---------------- 辅助 ---------------- */
/**
  * @brief  毫秒延时：调度器在跑时用 vTaskDelay，否则用 HAL_Delay
  * @note   业务任务里请用这个而不是 HAL_Delay，避免空转占满 CPU。
  */
void ADC_Drv_DelayMs(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* __ADC_DRV_H */
