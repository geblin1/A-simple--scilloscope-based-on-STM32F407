/**
  * @file    App_Rtos.c
  * @brief   把示波器拆成 FreeRTOS 任务并启动调度器
  *
  * 本文件只负责"谁在什么节拍跑"，不含任何业务逻辑：
  *   acq 任务 每 20 ms 起调一次 App_Scope_AcqStep()（内部阻塞等 DMA，最长约一帧）
  *   ui  任务 每 10 ms 调一次 App_Scope_UiStep()（绘制阻塞几十毫秒时自动降速）
  *   mon 任务 每 500 ms 刷新堆/栈余量与栈高水位（LED 心跳在 1 ms 中断里，不依赖任务），作为"系统还活着"的心跳
  *
  * 优先级说明见 App_Rtos.h 顶部。
  */

#include "App_Rtos.h"

#include "FreeRTOS.h"
#include "task.h"

#include "App_Scope.h"

/* ---------------- 任务参数 ---------------- */
#define APP_ACQ_PRIO        (tskIDLE_PRIORITY + 4U)     /* 采集最高：不能被绘图拖慢 */
#define APP_UI_PRIO         (tskIDLE_PRIORITY + 3U)
#define APP_MON_PRIO        (tskIDLE_PRIORITY + 1U)     /* 监控最低 */

/* 栈深单位是"字"(4 字节)。界面任务要跑 snprintf + 逐行刷屏，给足 2.5 KB。 */
#define APP_ACQ_STACK       (configMINIMAL_STACK_SIZE * 2U)     /* 1.0 KB */
#define APP_UI_STACK        (configMINIMAL_STACK_SIZE * 5U)     /* 2.5 KB */
#define APP_MON_STACK       (configMINIMAL_STACK_SIZE * 1U)     /* 0.5 KB */

/* ---------------- 节拍 ---------------- */
#define APP_ACQ_PERIOD_MS   20U     /* 采集最快 50 帧/秒；慢时基由采集本身决定 */
#define APP_UI_PERIOD_MS    10U     /* 按键/编码器扫描周期 */
#define APP_MON_PERIOD_MS   500U    /* 心跳周期 */

/* ---------------- 健康指标（调试器 Watch 观察） ---------------- */
volatile uint32_t g_monHeartbeat   = 0U;
volatile uint32_t g_monHeapFree    = 0U;
volatile uint32_t g_monAcqStackMin = 0U;
volatile uint32_t g_monUiStackMin  = 0U;
volatile uint32_t g_monUiFrames    = 0U;

static TaskHandle_t s_acq_task = NULL;
static TaskHandle_t s_ui_task  = NULL;

/**
  * @brief  采集任务：只管按节拍推进采集，不管画图
  */
static void App_AcqTask(void *argument)
{
    TickType_t last = xTaskGetTickCount();

    ( void ) argument;

    for( ; ; )
    {
        /* HOLD 时立即返回 0，不会阻塞；RUN 时内部阻塞在 DMA 完成信号量上，
           这期间 CPU 完全让给界面任务。 */
        if( App_Scope_AcqStep() != 0U )
        {
            /* 采完一帧必须显式让出 1 个 tick。
               xSemaphoreGive() 只把等待者变成就绪，不把所有权交给等待者：
               acq 优先级更高，如果这里不等待，下一轮 App_Scope_AcqStep()
               会立刻把 s_bus_lock 重新取走，界面任务永远抢不到锁——
               现象就是按 KEY4 开始采集后，按键/编码器要过好几秒才响应一次。
               让出 1 tick 后，界面任务必定能拿到锁并画完这一帧；
               绘制期间 acq 在锁上等待，和"采样与刷屏互斥"的约束一致。 */
            vTaskDelay( 1U );
        }

        /* 采一帧本身就慢于 20 ms 时，vTaskDelayUntil 会立刻返回（不空转），
           节奏自动由采集时长决定。 */
        vTaskDelayUntil( &last, pdMS_TO_TICKS( APP_ACQ_PERIOD_MS ) );
    }
}

/**
  * @brief  界面任务：输入扫描 + 重绘
  * @note   重绘是阻塞的 SPI 发送，所以本任务优先级低于采集任务
  */
static void App_UiTask(void *argument)
{
    TickType_t last = xTaskGetTickCount();
    uint8_t    drew;

    ( void ) argument;

    for( ; ; )
    {
        drew = App_Scope_UiStep();
        if( drew != 0U )
        {
            g_monUiFrames++;
        }

        /* 一帧要画几十毫秒，10 ms 周期根本用不完，vTaskDelayUntil 会立刻返回，
           于是本任务长期不阻塞，把优先级更低的监控任务（还有空闲任务）饿死——
           表现出来就是 LED 闪烁忽快忽慢。这里显式让出 1 个 tick：
           只损失约 3% 的绘制时间，换来低优先级任务按时执行。 */
        vTaskDelay( 1U );

        vTaskDelayUntil( &last, pdMS_TO_TICKS( APP_UI_PERIOD_MS ) );
    }
}

/**
  * @brief  监控任务：心跳 + 资源余量统计
  * @note   只读统计，不参与业务；任何情况下都不该影响前两个任务
  */
static void App_MonitorTask(void *argument)
{
    TickType_t last = xTaskGetTickCount();

    ( void ) argument;

    for( ; ; )
    {
        g_monHeartbeat++;

        /* 剩余堆：接近 0 说明 configTOTAL_HEAP_SIZE 该调大了 */
        g_monHeapFree = ( uint32_t ) xPortGetFreeHeapSize();

#if ( INCLUDE_uxTaskGetStackHighWaterMark == 1 )
        /* 栈最小余量（单位：字）。长期应大于 20，否则说明栈给小了 */
        g_monAcqStackMin = ( uint32_t ) uxTaskGetStackHighWaterMark( s_acq_task );
        g_monUiStackMin  = ( uint32_t ) uxTaskGetStackHighWaterMark( s_ui_task );
#endif

        /* LED 心跳已改由 1 ms 定时中断驱动（LED_Tick），
           任务被饿死时它依然准确，这里只做资源统计。 */

        vTaskDelayUntil( &last, pdMS_TO_TICKS( APP_MON_PERIOD_MS ) );
    }
}

void App_Rtos_Start(void)
{
    BaseType_t created = pdTRUE;

    if( xTaskCreate( App_AcqTask, "acq", APP_ACQ_STACK, NULL,
                     APP_ACQ_PRIO, &s_acq_task ) != pdPASS )
    {
        created = pdFALSE;
    }

    if( xTaskCreate( App_UiTask, "ui", APP_UI_STACK, NULL,
                     APP_UI_PRIO, &s_ui_task ) != pdPASS )
    {
        created = pdFALSE;
    }

    if( xTaskCreate( App_MonitorTask, "mon", APP_MON_STACK, NULL,
                     APP_MON_PRIO, NULL ) != pdPASS )
    {
        created = pdFALSE;
    }

    /* 堆不够时 configASSERT 会在 freertos_hooks.c 里挂起，
       用调试器看 g_ulRtosAssertLine / configTOTAL_HEAP_SIZE */
    configASSERT( created == pdTRUE );

    /* 启动调度器：正常情况下不返回 */
    vTaskStartScheduler();

    /* 只有空闲任务或定时器任务创建失败（堆耗尽）才会走到这里 */
    taskDISABLE_INTERRUPTS();
    for( ; ; )
    {
    }
}
