/**
  * @file    ADC_Drv.c
  * @brief   双 ADC（ADC1=PA0 / ADC2=PA1）同步采集驱动实现
  *
  * 本文件是整个工程里唯一碰 ADC / TIM2 / DMA2_Stream0 的地方。
  * 业务代码只通过 ADC_Drv.h 暴露的函数使用 ADC，不需要知道任何底层细节。
  *
  * FreeRTOS 兼容策略：
  *   - 完成通知用二值信号量，DMA 完成中断里用 xSemaphoreGiveFromISR；
  *   - DMA2_Stream0 的 NVIC 优先级 = 5 = configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY，
  *     属于“可调用 FreeRTOS FromISR API”的合法优先级区间；
  *   - 调度器没启动时（xTaskGetSchedulerState() != taskSCHEDULER_RUNNING）
  *     不碰任何 FreeRTOS 对象，退化为轮询 + HAL_Delay，裸机也能直接用；
  *   - 阻塞等待带超时，不会死等。
  */

#include "ADC_Drv.h"

#include "main.h"
#include "adc.h"
#include "tim.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* adc.c 里的 DMA 句柄，CubeMX 没有在 adc.h 里导出，这里自己声明 */
extern DMA_HandleTypeDef hdma_adc1;

/* TIM2 计数频率：84MHz / 84 = 1MHz，于是 ARR+1 就是 1 个采样点的微秒数 */
#define ADC_DRV_TIM_PSC_VALUE   (84U - 1U)
#define ADC_DRV_TIM_HZ          1000000U
#define ADC_DRV_POLL_TIMEOUT    20U          /* 轮询单次转换的超时（ms） */

/* ---------------- 模块状态 ---------------- */
static volatile uint8_t  s_busy;             /* 1 = 正在采集 */
static volatile uint8_t  s_done;             /* 1 = 本帧已搬完 */
static volatile uint8_t  s_error;            /* 1 = 本帧出错 */
static uint32_t          s_arr = 9U;         /* 当前 ARR（对应 100kSPS） */
static uint32_t          s_rate_hz = 100000U;
static SemaphoreHandle_t s_done_sem;         /* 帧完成信号量（可能为 NULL） */
static uint32_t          s_last_ndtr;        /* 停采集瞬间 DMA 剩余计数（诊断用） */

/* ---------------- 内部工具 ---------------- */
/** @brief 调度器是否已经在跑（ISR 里也安全，只读一个变量） */
static inline BaseType_t adc_drv_sched_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? pdTRUE : pdFALSE;
}

/** @brief 帧完成/出错时统一收尾：停触发、标记、唤醒等待者 */
static void adc_drv_frame_end(uint8_t error)
{
    BaseType_t woken = pdFALSE;

    /* 立刻停 TIM2，防止继续触发转换把 OVR 置起来 */
    (void)HAL_TIM_Base_Stop(&htim2);

    if (error != 0U)
    {
        s_error = 1U;
    }
    s_done = 1U;
    s_busy = 0U;

    if ((s_done_sem != NULL) && (adc_drv_sched_running() == pdTRUE))
    {
        (void)xSemaphoreGiveFromISR(s_done_sem, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

/* ---------------- 初始化 ---------------- */
ADC_Drv_Status ADC_Drv_Init(void)
{
    /* ADC2 是双 ADC 同步模式的从器件，必须使能；CubeMX 生成的 HAL_ADC_Init 不置 ADON */
    if ((hadc2.Instance->CR2 & ADC_CR2_ADON) == 0U)
    {
        __HAL_ADC_ENABLE(&hadc2);
        /* 等 tSTAB（手册约 3us），这里空转足够多的周期 */
        for (volatile uint32_t i = 0U; i < 300U; i++)
        {
            __NOP();
        }
    }

    /* 从器件 ADC2 的两个补丁：
       1) 清掉可能残留的 OVR —— F4 的 ADC 一旦 OVR 置位就会停止转换，
          现象正是“上电测了一次，之后数值永远不变”；
       2) 把主器件的触发配置复制给从器件。双 ADC 同步模式里从器件本该由主器件带动，
          但本板实测 ADC2 只转换了第一次，显式同源更稳妥。 */
    hadc2.Instance->SR = 0U;
    {
        uint32_t trig = hadc1.Instance->CR2 & (ADC_CR2_EXTSEL | ADC_CR2_EXTEN);
        hadc2.Instance->CR2 = (hadc2.Instance->CR2 & ~(ADC_CR2_EXTSEL | ADC_CR2_EXTEN)) | trig;
    }

    (void)HAL_TIM_Base_Stop(&htim2);
    hadc1.Instance->CR2 &= ~ADC_CR2_DMA;

    if (s_done_sem == NULL)
    {
        s_done_sem = xSemaphoreCreateBinary();   /* 调度器启动前创建也是合法的 */
    }

    s_busy    = 0U;
    s_done    = 0U;
    s_error   = 0U;
    s_rate_hz = 100000U;
    s_arr     = (ADC_DRV_TIM_HZ / s_rate_hz) - 1U;

    return ADC_DRV_OK;
}

/* ---------------- 单次采样 ---------------- */
ADC_Drv_Status ADC_Drv_ReadOnce(uint16_t *ch1, uint16_t *ch2)
{
    uint32_t saved_ccr;
    uint32_t saved_exten;
    uint32_t saved_exten2;
    uint32_t saved_dma;
    uint8_t  ok = 1U;

    if ((ch1 == NULL) || (ch2 == NULL))
    {
        return ADC_DRV_PARAM;
    }
    if (s_busy != 0U)
    {
        return ADC_DRV_BUSY;
    }

    /* 临时退出“双 ADC 同步 + 硬件触发”，改成两路各自软件启动轮询 */
    saved_ccr   = ADC->CCR;
    ADC->CCR    = saved_ccr & ~(ADC_CCR_MULTI | ADC_CCR_DMA);
    saved_exten = hadc1.Instance->CR2 & ADC_CR2_EXTEN;
    saved_dma   = hadc1.Instance->CR2 & ADC_CR2_DMA;
    /* 从器件 ADC2 的触发边沿也要一起清掉：
       ADC_Drv_Init() 把 ADC1 的 EXTSEL/EXTEN 复制给了 ADC2，若只清 ADC1，
       则 HAL_ADC_Start(&hadc2) 因 EXTEN != 0 不会置 SWSTART，ADC2 永远等不到转换。
       （HAL 只允许“无外部触发”时才软件启动，见 stm32f4xx_hal_adc.c:797） */
    saved_exten2 = hadc2.Instance->CR2 & ADC_CR2_EXTEN;
    /* CR2.DMA 必须清掉：EOCS+DMA 同时置位时 HAL_ADC_PollForConversion 会立即返回 HAL_ERROR */
    hadc1.Instance->CR2 &= ~(ADC_CR2_EXTEN | ADC_CR2_DMA);
    hadc2.Instance->CR2 &= ~(ADC_CR2_EXTEN | ADC_CR2_DMA);

    __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_EOC | ADC_FLAG_OVR);
    __HAL_ADC_CLEAR_FLAG(&hadc2, ADC_FLAG_EOC | ADC_FLAG_OVR);

    if ((HAL_ADC_Start(&hadc1) != HAL_OK) ||
        (HAL_ADC_PollForConversion(&hadc1, ADC_DRV_POLL_TIMEOUT) != HAL_OK))
    {
        ok = 0U;
    }
    else
    {
        *ch1 = (uint16_t)(hadc1.Instance->DR & 0xFFFFU);

        if ((HAL_ADC_Start(&hadc2) != HAL_OK) ||
            (HAL_ADC_PollForConversion(&hadc2, ADC_DRV_POLL_TIMEOUT) != HAL_OK))
        {
            ok = 0U;
        }
        else
        {
            *ch2 = (uint16_t)(hadc2.Instance->DR & 0xFFFFU);
        }
    }

    /* 原样恢复多 ADC 配置和触发边沿，不影响后续帧采集 */
    hadc1.Instance->CR2 |= (saved_exten | saved_dma);
    hadc2.Instance->CR2 |= saved_exten2;
    ADC->CCR = saved_ccr;

    return (ok != 0U) ? ADC_DRV_OK : ADC_DRV_TIMEOUT;
}

ADC_Drv_Status ADC_Drv_ReadOnceMv(uint16_t *mv1, uint16_t *mv2)
{
    uint16_t ch1 = 0U;
    uint16_t ch2 = 0U;
    ADC_Drv_Status st;

    if ((mv1 == NULL) || (mv2 == NULL))
    {
        return ADC_DRV_PARAM;
    }

    st = ADC_Drv_ReadOnce(&ch1, &ch2);
    if (st == ADC_DRV_OK)
    {
        *mv1 = ADC_Drv_ToMillivolt(ch1);
        *mv2 = ADC_Drv_ToMillivolt(ch2);
    }
    return st;
}

/* ---------------- 采样率 ---------------- */
ADC_Drv_Status ADC_Drv_SetSampleRate(uint32_t sps, uint32_t *actual)
{
    uint32_t div;

    if ((sps < ADC_DRV_RATE_MIN) || (sps > ADC_DRV_RATE_MAX))
    {
        return ADC_DRV_PARAM;
    }

    div = ADC_DRV_TIM_HZ / sps;
    if (div == 0U)
    {
        div = 1U;
    }

    s_arr     = div - 1U;
    s_rate_hz = ADC_DRV_TIM_HZ / (s_arr + 1U);

    if (actual != NULL)
    {
        *actual = s_rate_hz;
    }
    return ADC_DRV_OK;
}

uint32_t ADC_Drv_GetSampleRate(void)
{
    return s_rate_hz;
}

uint32_t ADC_Drv_GetMinSampleRate(void)
{
    return ADC_DRV_RATE_MIN;
}

uint32_t ADC_Drv_GetMaxSampleRate(void)
{
    return ADC_DRV_RATE_MAX;
}

uint32_t ADC_Drv_SamplesToUs(uint32_t samples)
{
    if (s_rate_hz == 0U)
    {
        return 0U;
    }
    return (uint32_t)(((uint64_t)samples * 1000000ULL) / (uint64_t)s_rate_hz);
}

/* ---------------- 帧采集 ---------------- */
ADC_Drv_Status ADC_Drv_CaptureStart(uint32_t *buf, uint32_t len)
{
    if ((buf == NULL) || (len == 0U) || (len > ADC_DRV_CAPTURE_MAX))
    {
        return ADC_DRV_PARAM;
    }

    /* 进入临界区保护 s_busy / s_done / s_error。
       注意：调度器没启动时**不能**用 taskENTER_CRITICAL()。
       FreeRTOS 的 uxCriticalNesting 初值是 0xAAAAAAAA，只有 xPortStartScheduler()
       才会把它清 0；调度器没起来时 taskEXIT_CRITICAL() 的计数永远回不到 0，
       portENABLE_INTERRUPTS() 就不会执行 —— PRIMASK 一直是 1，
       所有中断（TIM7 时基、DMA 完成）全部被关死。
       所以这里按"调度器是否在跑"分别用 FreeRTOS 临界区 / 裸 PRIMASK 保存-恢复。 */
    if (adc_drv_sched_running() == pdTRUE)
    {
        taskENTER_CRITICAL();

        if (s_busy != 0U)
        {
            taskEXIT_CRITICAL();
            return ADC_DRV_BUSY;
        }
        s_busy  = 1U;
        s_done  = 0U;
        s_error = 0U;

        taskEXIT_CRITICAL();
    }
    else
    {
        uint32_t primask = __get_PRIMASK();

        __disable_irq();
        if (s_busy != 0U)
        {
            __set_PRIMASK(primask);
            return ADC_DRV_BUSY;
        }
        s_busy  = 1U;
        s_done  = 0U;
        s_error = 0U;
        __set_PRIMASK(primask);
    }

    /* 若上一帧只查了 IsDone 没调 Stop，这里先彻底释放一次，保证可重启 */
    (void)HAL_ADCEx_MultiModeStop_DMA(&hadc1);
    (void)HAL_TIM_Base_Stop(&htim2);

    /* 丢掉可能残留的完成信号 */
    if (s_done_sem != NULL)
    {
        (void)xSemaphoreTake(s_done_sem, 0U);
    }

    __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_EOC | ADC_FLAG_OVR);
    /* 从器件同样要清：ADC2 一旦 OVR 置位就不再转换，CDR 的高 16 位会一直停在旧值 */
    hadc2.Instance->SR = 0U;

    /* 多 ADC 模式下 DMA 请求由主器件的 CR2.DMA 开门，HAL 不会替我们置这一位 */
    hadc1.Instance->CR2 |= ADC_CR2_DMA;

    if (HAL_ADCEx_MultiModeStart_DMA(&hadc1, buf, len) != HAL_OK)
    {
        hadc1.Instance->CR2 &= ~ADC_CR2_DMA;
        s_busy = 0U;
        return ADC_DRV_ERROR;
    }

    /* 先停表再改 ARR，避免 CNT 已经超过新 ARR 时要绕完整个 32 位计数才回卷 */
    __HAL_TIM_SET_PRESCALER(&htim2, ADC_DRV_TIM_PSC_VALUE);
    __HAL_TIM_SET_AUTORELOAD(&htim2, s_arr);
    __HAL_TIM_SET_COUNTER(&htim2, 0U);
    __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE);

    if (HAL_TIM_Base_Start(&htim2) != HAL_OK)
    {
        hadc1.Instance->CR2 &= ~ADC_CR2_DMA;
        (void)HAL_ADCEx_MultiModeStop_DMA(&hadc1);
        s_busy = 0U;
        return ADC_DRV_ERROR;
    }

    return ADC_DRV_OK;
}

ADC_Drv_Status ADC_Drv_CaptureWait(uint32_t timeout_ms)
{
    ADC_Drv_Status ret = ADC_DRV_TIMEOUT;

    if ((s_busy == 0U) && (s_done == 0U))
    {
        return ADC_DRV_ERROR;   /* 当前没有正在进行的采集 */
    }

    if ((s_done_sem != NULL) && (adc_drv_sched_running() == pdTRUE))
    {
        /* 有调度器：阻塞在信号量上，期间让出 CPU */
        if (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
        {
            ret = ADC_DRV_OK;
        }
    }
    else
    {
        /* 裸机：轮询 + 延时 */
        uint32_t start = HAL_GetTick();

        while (s_done == 0U)
        {
            /* 第二重完成判据：普通模式下 NDTR 归零就代表这一帧已经搬完。
               即使完成中断因为某种原因没进来，这里也能正确收尾。 */
            if (hdma_adc1.Instance->NDTR == 0U)
            {
                s_done = 1U;
                break;
            }
            if ((HAL_GetTick() - start) >= timeout_ms)
            {
                break;
            }
            ADC_Drv_DelayMs(1U);
        }
        if (s_done != 0U)
        {
            ret = ADC_DRV_OK;
        }
    }

    ADC_Drv_CaptureStop();

    /* 数据完整性优先：整帧搬满（NDTR 归零）就算成功。
       帧尾那一两个转换没有被 DMA 接收而置 OVR，只说明多发了一次触发，
       不影响已经搬进缓冲区的点，不应该把整帧判成失败——
       否则上层会一直认为"没采到"，出现"波形在动、测量值不动"的怪现象。 */
    if ((s_done != 0U) && (s_last_ndtr == 0U))
    {
        ret = ADC_DRV_OK;
    }
    else if ((ret == ADC_DRV_OK) && (s_error != 0U))
    {
        ret = ADC_DRV_ERROR;
    }
    return ret;
}

uint8_t ADC_Drv_CaptureIsDone(void)
{
    return s_done;
}

uint32_t ADC_Drv_CaptureRemaining(void)
{
    if (s_busy == 0U)
    {
        return 0U;
    }
    return (uint32_t)hdma_adc1.Instance->NDTR;
}

ADC_Drv_Status ADC_Drv_CaptureStop(void)
{
    /* 停之前先记下 DMA 还剩多少没搬（0 = 已搬满），供诊断使用 */
    s_last_ndtr = (uint32_t)hdma_adc1.Instance->NDTR;

    (void)HAL_TIM_Base_Stop(&htim2);
    (void)HAL_ADCEx_MultiModeStop_DMA(&hadc1);
    hadc1.Instance->CR2 &= ~ADC_CR2_DMA;
    s_busy = 0U;
    return ADC_DRV_OK;
}

uint32_t ADC_Drv_GetLastRemaining(void)
{
    return s_last_ndtr;
}

/* ---------------- 辅助 ---------------- */
void ADC_Drv_DelayMs(uint32_t ms)
{
    if (ms == 0U)
    {
        return;
    }
    if (adc_drv_sched_running() == pdTRUE)
    {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    else
    {
        HAL_Delay(ms);
    }
}

/* ---------------- HAL 回调（本驱动独占，业务代码不要重复实现） ---------------- */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if ((hadc == NULL) || (hadc->Instance != ADC1))
    {
        return;
    }
    adc_drv_frame_end(0U);
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    if ((hadc == NULL) || (hadc->Instance != ADC1))
    {
        return;
    }
    adc_drv_frame_end(1U);
}
