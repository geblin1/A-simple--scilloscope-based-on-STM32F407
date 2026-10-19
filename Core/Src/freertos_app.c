/**
  ******************************************************************************
  * @file    freertos_app.c
  * @brief   FreeRTOS 应用层入口与示例任务
  *
  * 对应参考工程 D:\平衡车\平衡车程序\Core\Src\freertos_demo.c（freertos_start()）。
  * 本文件不属于 STM32CubeMX 管理范围，重新生成代码不会影响它。
  *
  * 三个示例任务演示了本工程后续要用的三种机制：
  *   生产者任务  AppProducerTask  —— 周期性采集（未来的示波器 ADC 采样）
  *   消费者任务  AppConsumerTask  —— 从队列取数据处理（未来的波形/测量计算）
  *   秒任务      AppOneSecondTask —— 低频状态刷新（未来的万用表读数、界面刷新）
  * 外加一个软件定时器 AppTimerCallback（未来的信号发生器波形换表节拍）。
  *
  * 注意：本工程当前在 CubeMX 里没有配置任何 GPIO 引脚（Core/Src/gpio.c 的
  *       MX_GPIO_Init() 只开了 GPIOH/GPIOA 时钟），所以示例任务不做 LED 闪烁，
  *       而是用全局计数器供调试器观察。等你在 CubeMX 里配上 LED 引脚后，直接
  *       在对应任务里调用 HAL_GPIO_TogglePin() 即可（注意 HAL 时基是 TIM7，
  *       HAL_Delay() 在 RTOS 起来后会被任务调度打断，任务里请用 vTaskDelay()）。
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"

#include "freertos_app.h"

/* 调试器观察用的运行时统计量 ------------------------------------------------*/
volatile uint32_t g_ulRtosProducedCount  = 0U;
volatile uint32_t g_ulRtosConsumedCount  = 0U;
volatile uint32_t g_ulRtosSecondsCount   = 0U;
volatile uint32_t g_ulRtosTimerCount     = 0U;
volatile uint32_t g_ulRtosLastSample     = 0U;
volatile uint32_t g_ulRtosHeapFreeBytes  = 0U;

/* 示例参数 ------------------------------------------------------------------*/
#define APP_QUEUE_LENGTH      8U                                        /* 采样队列深度 */
#define APP_TASK_STACK        ( configMINIMAL_STACK_SIZE * 2U )         /* 每任务 1 KB 栈 */
#define APP_TASK_PRIORITY     ( tskIDLE_PRIORITY + 2U )                 /* 示例任务优先级 */
#define APP_SAMPLE_PERIOD_MS  500U                                      /* 采样周期 */
#define APP_REPORT_PERIOD_MS  1000U                                     /* 秒任务周期 */

/* 采样数据结构：未来把 ulValue 换成 ADC 采样值即可 -------------------------*/
typedef struct
{
    uint32_t ulTick;        /* 采样时刻（RTOS tick） */
    uint32_t ulSequence;    /* 序号，用于检查是否丢包 */
    uint32_t ulValue;       /* 采样值 */
} AppSample_t;

/* 模块私有句柄 --------------------------------------------------------------*/
static QueueHandle_t s_xSampleQueue = NULL;
static TimerHandle_t s_xSampleTimer = NULL;

/* 私有函数声明 --------------------------------------------------------------*/
static void AppProducerTask( void * pvParameters );
static void AppConsumerTask( void * pvParameters );
static void AppOneSecondTask( void * pvParameters );
static void AppTimerCallback( TimerHandle_t xTimer );

/**
  * @brief  生产者任务：周期性产生一条"采样"并投递到队列。
  */
static void AppProducerTask( void * pvParameters )
{
    AppSample_t     xSample;
    TickType_t      xLastWakeTime;
    uint32_t        ulSequence = 0U;
    BaseType_t      xStatus;

    ( void ) pvParameters;

    xLastWakeTime = xTaskGetTickCount();

    for( ; ; )
    {
        ulSequence++;

        xSample.ulTick     = ( uint32_t ) xTaskGetTickCount();
        xSample.ulSequence = ulSequence;
        xSample.ulValue    = ulSequence * 2U;      /* 占位数据：接 ADC 后替换 */

        /* 队列满时最多等 10 ms，不阻塞采样节拍 */
        xStatus = xQueueSend( s_xSampleQueue, &xSample, pdMS_TO_TICKS( 10 ) );
        if( xStatus == pdPASS )
        {
            g_ulRtosProducedCount = ulSequence;
        }

        vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS( APP_SAMPLE_PERIOD_MS ) );
    }
}

/**
  * @brief  消费者任务：阻塞等队列，取到数据后处理。
  */
static void AppConsumerTask( void * pvParameters )
{
    AppSample_t xSample;

    ( void ) pvParameters;

    for( ; ; )
    {
        /* 无数据时阻塞，不空转 CPU；portMAX_DELAY 表示一直等 */
        if( xQueueReceive( s_xSampleQueue, &xSample, portMAX_DELAY ) == pdPASS )
        {
            g_ulRtosLastSample = xSample.ulValue;
            g_ulRtosConsumedCount++;
            /* 未来在这里做波形重建 / 有效值 / 频率计算 */
        }
    }
}

/**
  * @brief  秒任务：低频状态刷新，顺带刷新剩余堆空间。
  */
static void AppOneSecondTask( void * pvParameters )
{
    TickType_t xLastWakeTime;

    ( void ) pvParameters;

    xLastWakeTime = xTaskGetTickCount();

    for( ; ; )
    {
        g_ulRtosSecondsCount++;

        /* 剩余堆（字节）：用于判断 configTOTAL_HEAP_SIZE 是否够用 */
        g_ulRtosHeapFreeBytes = ( uint32_t ) xPortGetFreeHeapSize();

        vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS( APP_REPORT_PERIOD_MS ) );
    }
}

/**
  * @brief  软件定时器回调：运行在定时器服务任务里（不是中断）。
  *         未来信号发生器用它按节拍切换波形查找表。
  */
static void AppTimerCallback( TimerHandle_t xTimer )
{
    ( void ) xTimer;

    g_ulRtosTimerCount++;
}

/**
  * @brief  创建 RTOS 对象、创建任务、启动调度器。
  */
void freertos_app_start( void )
{
    /* 1. 队列：生产者 -> 消费者 */
    s_xSampleQueue = xQueueCreate( APP_QUEUE_LENGTH, sizeof( AppSample_t ) );
    configASSERT( s_xSampleQueue != NULL );

    /* 2. 任务 */
    xTaskCreate( AppProducerTask   , "producer", APP_TASK_STACK, NULL,
                 APP_TASK_PRIORITY, NULL );
    xTaskCreate( AppConsumerTask   , "consumer", APP_TASK_STACK, NULL,
                 APP_TASK_PRIORITY, NULL );
    xTaskCreate( AppOneSecondTask  , "one_sec" , APP_TASK_STACK, NULL,
                 tskIDLE_PRIORITY + 1U, NULL );

    /* 3. 软件定时器（周期 1 s，自动重载） */
    s_xSampleTimer = xTimerCreate( "app_timer", pdMS_TO_TICKS( APP_REPORT_PERIOD_MS ),
                                   pdTRUE, NULL, AppTimerCallback );
    configASSERT( s_xSampleTimer != NULL );
    ( void ) xTimerStart( s_xSampleTimer, 0U );

    /* 4. 启动调度器：正常情况下不返回 */
    vTaskStartScheduler();

    /* 只有动态分配空闲任务/定时器任务失败时才会走到这里 */
    for( ; ; )
    {
    }
}
