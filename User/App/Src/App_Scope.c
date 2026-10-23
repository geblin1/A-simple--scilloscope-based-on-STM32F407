/**
  * @file    App_Scope.c
  * @brief   示波器应用层：输入处理 + 采集 + 状态 + 重绘
  *
  * 界面按键约定（MAIN 模式）
  * ------------------------
  *   KEY1 切换当前通道（参数栏测量 + 触发源 + 编码器1 的 V/div + 标定目标）
  *   KEY2 进入设置菜单
  *   KEY3 切换显示模式（双路 / 仅 CH1 / 仅 CH2 / FFT 频谱）
  *   KEY4 运行 / 停止
  *   编码器1  调当前通道的 V/div（两路各自独立）
  *   编码器2  调 Time/div
  *
  * 菜单模式
  * --------
  *   编码器1 上下移动选中行，编码器2 改数值，KEY2 退出
  *
  *   可选行：V/div（当前通道，行内同时显示两路值）、Time/div、触发源、触发边沿、
 *          触发电平、自动设置（Auto set；KEY4 执行）、标定（Cal；KEY4 启动）
 *   KEY1 在菜单里任何一行都切换当前通道（编码器1 与 Cal 行都跟着走）。
 *
 * 触发（TRIGGER）
 * --------------
 *   不触发时每帧的采集起点是随机的，周期信号在屏幕上会左右游走。
 *   开启触发后每帧多采一点（一屏 + 触发点左右各 160 点），在整段里找
 *   "按指定边沿穿过触发电平"的那一点，把这一点固定到屏幕中线，
 *   于是相邻帧相位一致，波形就"钉"住了。
 *   触发点还要用亚采样插值对齐：只对齐到采样网格的话，真实穿越时刻与
 *   采样点的位置差每帧都不同，波形会横向差 ±1 像素，增量刷新就失效了。
 *   找不到触发点时不冻结画面，而是退回不触发的显示方式，并用红色虚线
 *   标出"当前没有触发"，提示用户去菜单里调电平 / 边沿。
 *   指定电平找不到触发点时，会再自动取"信号中点"试一次（AUTO 电平）：
 *   单极性信号（PWM、逻辑电平）在默认 0 mV 下永远凑不出穿越条件，
 *   AUTO 让这类信号接上就能钉住；成功后电平会写回状态，菜单数值和
 *   屏幕上的电平线同步跟到实际触发位置。
 *
 * 频谱（FFT）
 *
 *   显示模式切到 FFT 时，对"当前通道"的 320 点显示窗取中间 256 点做 FFT
 *   （Hann 窗、Q15 定点，见 Algorithm/Fft.c），把 0..采样率/2 的幅度谱
 *   （相对最强谱线的 dB）画在波形区。状态栏 V/div 位置显示 "FFT"、
 *   Time 位置改成"频率/格"（= 采样率/16）；参数栏仍是时域测量值，
 *   键和编码器语义完全不变。
 *   频谱被合成为一条 320 点"假波形"（y = 117 - mv/100，vdiv 固定 1900），
 *   直接复用 WaveView 的增量刷新 / 行指纹 / 网格，不必另写一套绘制，
 *   所以切到频谱也不会掉回整片重画那种低帧率。
 *
 * 运行模型（FreeRTOS）
  * -------------------
  *   本文件不再提供 while(1) 主循环，而是拆成两个"步进函数"：
  *     App_Scope_AcqStep()  由采集任务调用：采一帧 -> 换算 -> 测量 -> 发布
  *     App_Scope_UiStep()   由界面任务调用：读输入 -> 改状态 -> 取新帧 -> 重绘
  *   任务划分与优先级见 App_Rtos.c。
  *
  * 共享数据的保护
  * -------------
  *   s_st    ：界面任务写、采集任务读。字段都是 8/32 位对齐标量，
  *             Cortex-M4 上单次读写天然原子；最坏情况是采集任务多用了
  *             一拍旧参数，下一帧自动跟上。因此不加锁，避免长时间持锁。
  *   s_frame ：采集任务 -> 界面任务的整帧交接，用互斥量保护。两侧都只在
  *             "拷贝 2.5 KB"这一小段时间内持锁，不会阻塞绘制。
  */

#include "App_Scope.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "ADC_Drv.h"
#include "EncoderDrv.h"
#include "Fft.h"
#include "Frontend.h"
#include "Key.h"
#include "Lcd.h"
#include "Measure.h"
#include "ScopeMenu.h"
#include "ScopeView.h"
#include "Trigger.h"

/* 横屏一屏 320 列，一列一个采样点 */
#define APP_CAP_LEN         320U

/* 触发窗口：比一屏多采一些，触发点固定放在屏幕中线。
   APP_TRIG_PRE   = 中线左侧样本数（一屏的一半）
   APP_TRIG_POST  = 中线右侧样本数（必须凑满一屏）
   APP_CAP_RAW    = 整段采集长度 = 一屏 + 左右两侧
   例：320 + 160 + 160 = 640 点。多采的一倍数据只用于找触发点，
   显示时只把其中连续 320 点交给界面任务。 */
#define APP_TRIG_PRE        160U
#define APP_TRIG_POST       (APP_CAP_LEN - APP_TRIG_PRE)
#define APP_CAP_RAW         (APP_CAP_LEN + APP_TRIG_PRE + APP_TRIG_POST)

#if (APP_TRIG_PRE >= APP_CAP_LEN)
#error "APP_TRIG_PRE 必须小于一屏宽度"
#endif

#if (APP_CAP_RAW > ADC_DRV_CAPTURE_MAX)
#error "APP_CAP_RAW 超过 DAC/ADC 驱动单次采集上限"
#endif

#define APP_VDIV_COUNT      7U
#define APP_TDIV_COUNT      12U

/* V/div 与前端量程的对应关系
   --------------------------
   软件检测不到跳线乙，所以用 V/div 档位间接指定量程（两路各自独立）：
     前 4 档（100 / 200 / 500 / 1000 mV/div）-> 小量程，跳线乙插 MID
     后 3 档（2000 / 5000 / 10000 mV/div）   -> 大量程，跳线乙不插
   状态栏会分别显示两路各自的 S / W，提示用户该把对应通道的跳线放哪一档。
   跨过这个分界时波形会突然变形，那是正常的——说明该通道的跳线位置和新档位对不上。 */
#define APP_VDIV_SMALL_MAX  3U

/* 显示模式（KEY3 循环）：双路 / 仅 CH1 / 仅 CH2 / FFT 频谱 */
#define APP_SHOW_BOTH       0U
#define APP_SHOW_CH1        1U
#define APP_SHOW_CH2        2U
#define APP_SHOW_FFT        3U

/* 频谱模式用来把 dB 映射成像素的"假 V/div"。
   WaveView 的换算 y = 117 - mv*19/vdiv，取 1900 时正好是 y = 117 - mv/100，
   App_BuildSpectrum() 按这个关系反推 mV 即可；不参与任何用户可见档位。 */
#define APP_FFT_VDIV        1900U

static const uint16_t k_vdiv_mv[APP_VDIV_COUNT] =
{
    100U, 200U, 500U, 1000U,        /* 小量程（跳线乙插 MID） */
    2000U, 5000U, 10000U            /* 大量程（跳线乙不插）   */
};

static const uint32_t k_tdiv_us[APP_TDIV_COUNT] =
{
    20U, 50U, 100U, 200U, 500U, 1000U,
    2000U, 5000U, 10000U, 20000U, 50000U, 100000U
};

typedef struct
{
    uint8_t  measure_ch;    /* 参数栏测哪个通道：1 / 2 */
    uint8_t  show_mode;     /* APP_SHOW_xxx */
    uint8_t  vdiv_idx[2];   /* [0]=CH1、[1]=CH2 各自独立的垂直档位 */
    uint8_t  tdiv_idx;
    uint8_t  running;
    uint8_t  rate_limited;
    uint8_t  menu;
    uint8_t  menu_sel;
    uint8_t  trig_src;       /* 触发通道：1 / 2 */
    uint8_t  trig_edge;      /* Trigger_Edge：关 / 上升 / 下降 */
    int32_t  trig_level_mv;  /* 触发电平（mV，带符号） */
    uint32_t sps;
    uint32_t tdiv_us;
} App_State;

/* 一帧的可显示内容：两路已标定的 mV 值 + 两路各自的测量结果 + 当帧采样率 */
typedef struct
{
    int32_t        mv1[APP_CAP_LEN];
    int32_t        mv2[APP_CAP_LEN];
    Measure_Result meas[2];  /* [0] = CH1，[1] = CH2：HOLD 时切通道也能立刻显示对应结果 */
    uint32_t       sps;
    uint32_t       seq;      /* 发布序号，0 表示还没有任何一帧 */
    uint8_t        trig;     /* 本帧触发状态，见 APP_TRIG_STATE_xxx */
} Scope_Frame;

/* 本帧触发状态（随帧发布，界面任务据此画触发电平标记） */
#define APP_TRIG_STATE_OFF      0U   /* 触发关闭：不画标记 */
#define APP_TRIG_STATE_LOCKED   1U   /* 找到触发点：灰色虚线 */
#define APP_TRIG_STATE_MISSING  2U   /* 没找到触发点：红色虚线 */

static App_State       s_st;
static uint32_t        s_raw[APP_CAP_RAW];   /* 采集任务私有：DMA 目标缓冲 */
static int32_t         s_mv1[APP_CAP_RAW];   /* 采集任务私有：换算后的 mV（整段） */
static int32_t         s_mv2[APP_CAP_RAW];

static Scope_Frame     s_frame;              /* 采集 -> 界面 的交接帧（互斥量保护） */
static Scope_Frame     s_view;               /* 界面任务私有：绘图用的帧副本 */
static SemaphoreHandle_t s_lock;             /* 保护 s_frame / s_have_frame */
static SemaphoreHandle_t s_bus_lock;         /* 屏刷(SPI) 与 ADC 采集互斥 */
static uint8_t         s_have_frame;         /* 至少发布过一帧（互斥量保护） */
static uint8_t         s_draw_pending;       /* 有一帧欠着没画（界面任务私有） */
static uint8_t         s_auto_pass;          /* AUTO 还要在新帧上再算几遍（界面私有） */
static Fft_Result      s_fft;                /* 界面私有：最近一次频谱结果 */
static uint32_t        s_fft_seq;            /* s_fft 对应的帧序号（0 = 还没算过） */
static int32_t         s_spec[APP_CAP_LEN];  /* 频谱合成为 320 点"假波形"的 mV 刻度 */

/* ---- 前端标定（菜单 Cal 行 -> KEY4 启动；两点法，逐通道逐量程） ---- */
#define APP_CAL_IDLE        0U   /* 不在标定模式 */
#define APP_CAL_GND         1U   /* 等用户接 GND 并按 KEY4 */
#define APP_CAL_SPAN        2U   /* 等用户接 3.3 V 并按 KEY4 */
#define APP_CAL_DONE        3U   /* 标定完成，停留一会儿再回菜单 */
#define APP_CAL_SAMPLES     64U  /* 每个点连采多少次取平均 */
#define APP_CAL_DONE_MS     1500U/* 完成提示停留时间 */

static uint8_t    s_cal_step = APP_CAL_IDLE;  /* 标定流程状态，见 APP_CAL_xxx */
static uint8_t    s_cal_ch   = FRONTEND_CH1;  /* 本次要标定的通道 */
static uint8_t    s_cal_resume_run;           /* 进入标定前是否在 RUN，退出时恢复 */
static uint16_t   s_cal_zero_raw;             /* GND 实测码值 */
static TickType_t s_cal_deadline;             /* 完成提示的停留截止时刻 */

/* 采集期间是否禁止刷屏（默认开启）。
   整屏 SPI 突发会在 3.3 V 与地上打出电流尖峰；如果此时 DMA 正在采样，
   尖峰会经前端偏置网络耦合进 ADC 输入，画面上就表现为"噪声大"。
   裸机版本天然不会重叠（先采完 320 点、再画），上了 RTOS 后采集与刷屏
   是两个任务、并行发生，必须显式互斥才能恢复原来的采样环境。
   置 0 可以复现"并行"状态，用于对比噪声。 */
#define APP_SERIALIZE_ACQ_DRAW 1U

static void App_BusLock(void)
{
#if (APP_SERIALIZE_ACQ_DRAW != 0U)
    if (s_bus_lock != NULL)
    {
        (void)xSemaphoreTake(s_bus_lock, portMAX_DELAY);
    }
#endif
}

static void App_BusUnlock(void)
{
#if (APP_SERIALIZE_ACQ_DRAW != 0U)
    if (s_bus_lock != NULL)
    {
        (void)xSemaphoreGive(s_bus_lock);
    }
#endif
}

/* 带超时的取锁（界面任务专用）。锁被采集任务占着时最多等 timeout_ms，
   等不到就先回去扫按键/编码器，把"欠一帧"留到下一轮。
   没有它的话，慢时基下一帧采集要几百毫秒，按键输入会被一起拖住。 */
static uint8_t App_BusLockWait(uint32_t timeout_ms)
{
#if (APP_SERIALIZE_ACQ_DRAW != 0U)
    if (s_bus_lock == NULL)
    {
        return 1U;
    }

    return (xSemaphoreTake(s_bus_lock, pdMS_TO_TICKS(timeout_ms)) == pdPASS)
               ? 1U : 0U;
#else
    (void)timeout_ms;
    return 1U;
#endif
}

/* ------------------------------------------------------------------ */
/* 前端量程（由 V/div 档位决定，见 k_vdiv_mv 上方说明）                */
/* ------------------------------------------------------------------ */

/* 某一通道当前的 V/div（mV/格）。channel 用 FRONTEND_CH1 / FRONTEND_CH2 */
static uint16_t App_VdivMv(uint8_t channel)
{
    uint8_t idx = (channel == FRONTEND_CH2) ? 1U : 0U;

    return k_vdiv_mv[s_st.vdiv_idx[idx]];
}

/* 触发相关的一切（迟滞、±5 格限制、电平步进）都跟触发通道的 V/div 走 */
static uint16_t App_TrigVdivMv(void)
{
    return App_VdivMv(s_st.trig_src);
}

static void App_ApplyRanges(void)
{
    Frontend_SetRange(FRONTEND_CH1,
                      (s_st.vdiv_idx[0] <= APP_VDIV_SMALL_MAX)
                          ? FRONTEND_RANGE_SMALL
                          : FRONTEND_RANGE_WIDE);
    Frontend_SetRange(FRONTEND_CH2,
                      (s_st.vdiv_idx[1] <= APP_VDIV_SMALL_MAX)
                          ? FRONTEND_RANGE_SMALL
                          : FRONTEND_RANGE_WIDE);
}

/* ------------------------------------------------------------------ */
/* 时基                                                                */
/* ------------------------------------------------------------------ */

/* 40 个像素对应一格 -> 采样率 = 40 / (Time/div) */
#define APP_SPS_NUM     40000000UL

static void App_Timebase(void)
{
    uint32_t want;
    uint32_t actual = 0U;
    uint8_t  limited = 0U;

    want = APP_SPS_NUM / k_tdiv_us[s_st.tdiv_idx];

    if (want > ADC_DRV_RATE_MAX)
    {
        want    = ADC_DRV_RATE_MAX;
        limited = 1U;
    }
    if (want < ADC_DRV_RATE_MIN)
    {
        want    = ADC_DRV_RATE_MIN;
        limited = 1U;
    }

    if (ADC_Drv_SetSampleRate(want, &actual) != ADC_DRV_OK)
    {
        actual = ADC_Drv_GetSampleRate();
    }
    if (actual == 0U)
    {
        actual = ADC_DRV_RATE_MIN;
    }

    s_st.sps          = actual;
    s_st.tdiv_us      = APP_SPS_NUM / actual;
    s_st.rate_limited = limited;
}

/* ------------------------------------------------------------------ */
/* 触发                                                                */
/* ------------------------------------------------------------------ */

/* 迟滞取 1/20 格（一格的 5%）：太小会被噪声反复穿越，太大又难触发。
   至少 1 mV，否则整型除法会把小档位压成 0，退化成单阈值比较。 */
static void App_TriggerConfig(Trigger_Config *cfg)
{
    int32_t hyst = (int32_t)App_TrigVdivMv() / 20;

    if (hyst < 1)
    {
        hyst = 1;
    }

    cfg->level_mv      = s_st.trig_level_mv;
    cfg->hysteresis_mv = hyst;
    cfg->edge          = (Trigger_Edge)s_st.trig_edge;
    cfg->pre_samples   = (uint16_t)APP_TRIG_PRE;
    cfg->post_samples  = (uint16_t)APP_TRIG_POST;
}

/* 触发电平限制在屏幕上下各 5 格以内：再往外就永远碰不到波形，
   只会让标记停在边框上，没有意义。 */
static void App_ClampLevel(void)
{
    int32_t lim = (int32_t)App_TrigVdivMv() * 5;

    if (s_st.trig_level_mv > lim)
    {
        s_st.trig_level_mv = lim;
    }
    if (s_st.trig_level_mv < -lim)
    {
        s_st.trig_level_mv = -lim;
    }
}

/* 编码器改触发边沿：关 -> 上升 -> 下降 -> 关，循环 */
static void App_StepEdge(int16_t delta)
{
    int16_t v = (int16_t)s_st.trig_edge + delta;

    while (v < 0)
    {
        v += 3;
    }
    s_st.trig_edge = (uint8_t)(v % 3);
}

/* ------------------------------------------------------------------ */
/* 采集                                                                */
/* ------------------------------------------------------------------ */

/* 一帧的总时长 = APP_CAP_RAW / sps（含触发窗口）。慢时基下（1 kSa/s）
   要 640 ms，固定 60 ms 超时会让每一帧都判超时，所以这里按采样率算超时。 */
static uint32_t App_CaptureTimeoutMs(uint32_t sps)
{
    uint32_t ms;

    if (sps == 0U)
    {
        return 500U;
    }

    ms = (APP_CAP_RAW * 1000U) / sps;   /* 理论采集时长（含触发窗口） */
    ms += 100U;                          /* 中断/调度余量 */
    if (ms > 1500U)
    {
        ms = 1500U;
    }
    return ms;
}

/* 把一窗 320 点重采样到"真实穿越时刻"为起点（亚采样线性插值）。
   index/frac_q16 来自 Trigger_Find：穿越发生在 buf[index] 与 buf[index+1] 之间。
   输出第 i 点对应 (index - PRE + i) + frac 个采样周期，
   于是第 160 点恰好落在穿越时刻（屏幕中线），触发点不再随采样网格跳动。
   就地覆盖是安全的：源下标 a = index - PRE + i 满足 a >= i（index >= PRE）。 */
static void App_ResampleWindow(int32_t *buf, uint16_t index, uint16_t frac_q16)
{
    uint32_t i;
    uint32_t a;
    int32_t  v0;
    int32_t  v1;

    for (i = 0U; i < APP_CAP_LEN; i++)
    {
        a  = (uint32_t)index - (uint32_t)APP_TRIG_PRE + i;
        v0 = buf[a];
        v1 = buf[a + 1U];

        buf[i] = v0 + (int32_t)(((int64_t)(v1 - v0) * (int32_t)frac_q16) /
                                (int64_t)65536);
    }
}

/* AUTO 电平：取整段采集里信号最小值/最大值的中间点。
   单极性信号（PWM、逻辑电平）最低就是 0 V，在默认 0 mV 电平下"先低于
   电平减迟滞"永远不成立，触发条件根本不可能满足；取中点则总能落在幅度
   内，用户不用先手动调电平。信号是纯直流时取到的也是直流值，仍然找不到
   穿越点，会照常回退到自由采样（红线提示）。 */
static int32_t App_AutoLevel(const int32_t *mv)
{
    int32_t  lo = mv[0];
    int32_t  hi = mv[0];
    uint16_t i;

    for (i = 1U; i < APP_CAP_RAW; i++)
    {
        if (mv[i] < lo)
        {
            lo = mv[i];
        }
        if (mv[i] > hi)
        {
            hi = mv[i];
        }
    }

    return (lo + hi) / 2;
}

/* 采一整段 -> 换算成 mV -> 找触发点 -> 对齐显示窗口。
   *win_start  显示窗口在 s_mv1/s_mv2 里的起点：
                触发锁定时为 0（两路已就地重采样，第 160 点就是穿越时刻）；
                没触发 / 触发关闭时是整段里的裸窗口起点。
   *trig_state APP_TRIG_STATE_xxx
   返回 1 表示 DMA 采集正常完成（超时也照样换算，画面不会出现假 0 线）。 */
static uint8_t App_Capture(uint32_t sps, uint16_t *win_start, uint8_t *trig_state)
{
    uint16_t i;
    uint8_t  status;
    uint32_t start = (uint32_t)APP_TRIG_PRE;
    uint8_t  state = APP_TRIG_STATE_OFF;

    status = (uint8_t)ADC_Drv_CaptureStart(s_raw, APP_CAP_RAW);
    if (status == (uint8_t)ADC_DRV_OK)
    {
        status = (uint8_t)ADC_Drv_CaptureWait(App_CaptureTimeoutMs(sps));
        (void)ADC_Drv_CaptureStop();
    }

    /* 不管成功还是超时都换算一遍：超时时缓冲区里可能是上一次的旧数据，
       但至少不会让画面上出现全 0 的假直线。 */
    for (i = 0U; i < APP_CAP_RAW; i++)
    {
        s_mv1[i] = Frontend_RawToMv(FRONTEND_CH1, ADC_Drv_WordCh1(s_raw[i]));
        s_mv2[i] = Frontend_RawToMv(FRONTEND_CH2, ADC_Drv_WordCh2(s_raw[i]));
    }

    if (s_st.trig_edge != (uint8_t)TRIGGER_EDGE_OFF)
    {
        Trigger_Config  cfg;
        Trigger_Point   tp;
        const int32_t  *ch;

        App_TriggerConfig(&cfg);
        ch = (s_st.trig_src == 2U) ? s_mv2 : s_mv1;

        state = APP_TRIG_STATE_MISSING;      /* 先假定找不到 */
        tp    = Trigger_Find(ch, (uint16_t)APP_CAP_RAW, &cfg);

        /* AUTO 电平：指定电平找不到触发点时，再取信号中点试一次。
           单极性 PWM / 逻辑信号在默认 0 mV 下永远凑不出"先低于电平-迟滞"，
           没有这一步用户必须先进菜单把电平调进幅度范围。成功时把实际用到
           的电平写回状态，菜单里的数值和屏幕上的电平线会一起跟过去。 */
        if (tp.index < 0)
        {
            int32_t auto_level = App_AutoLevel(ch);

            if (auto_level != cfg.level_mv)
            {
                cfg.level_mv = auto_level;
                tp = Trigger_Find(ch, (uint16_t)APP_CAP_RAW, &cfg);
                if (tp.index >= 0)
                {
                    s_st.trig_level_mv = auto_level;
                }
            }
        }

        if (tp.index >= 0)
        {
            /* 用亚采样位置把两路都重采样到同一个时间原点：触发时刻正好
               落在显示窗第 160 点（屏幕中线）。这样相邻帧的波形只差 ADC
               噪声，增量刷新的行指纹能跳过绝大部分行，刷新率才不会被
               触发的 ±1 像素相位抖动拖回"整屏重画"的水平。
               重采样后显示窗口固定从 0 开始。 */
            App_ResampleWindow(s_mv1, (uint16_t)tp.index, tp.frac_q16);
            App_ResampleWindow(s_mv2, (uint16_t)tp.index, tp.frac_q16);
            start = 0U;
            state = APP_TRIG_STATE_LOCKED;
        }
    }

    *win_start  = (uint16_t)start;
    *trig_state = state;

    return (status == (uint8_t)ADC_DRV_OK) ? 1U : 0U;
}

/* 把本帧交给界面任务：持锁只做内存拷贝，不碰任何硬件。
   win_start 是显示窗口在整段采集里的起点（触发点左侧），
   只发布这一窗 320 点，界面层不需要知道触发搜索的存在。 */
static void App_PublishFrame(const Measure_Result meas[2], uint32_t sps,
                             uint16_t win_start, uint8_t trig_state)
{
    if (s_lock == NULL)
    {
        return;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdPASS)
    {
        return;
    }

    (void)memcpy(s_frame.mv1, &s_mv1[win_start], sizeof(s_frame.mv1));
    (void)memcpy(s_frame.mv2, &s_mv2[win_start], sizeof(s_frame.mv2));
    s_frame.meas[0] = meas[0];
    s_frame.meas[1] = meas[1];
    s_frame.sps  = sps;
    s_frame.trig = trig_state;
    s_frame.seq++;
    s_have_frame = 1U;

    (void)xSemaphoreGive(s_lock);
}

/* 界面任务取最新一帧；返回 1 表示拿到了比上次更新的帧 */
static uint8_t App_PullFrame(void)
{
    uint8_t got = 0U;

    if (s_lock == NULL)
    {
        return 0U;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdPASS)
    {
        return 0U;
    }

    if ((s_have_frame != 0U) && (s_frame.seq != s_view.seq))
    {
        (void)memcpy(s_view.mv1, s_frame.mv1, sizeof(s_view.mv1));
        (void)memcpy(s_view.mv2, s_frame.mv2, sizeof(s_view.mv2));
        s_view.meas[0] = s_frame.meas[0];
        s_view.meas[1] = s_frame.meas[1];
        s_view.sps  = s_frame.sps;
        s_view.trig = s_frame.trig;
        s_view.seq  = s_frame.seq;
        got = 1U;
    }

    (void)xSemaphoreGive(s_lock);

    return got;
}

/* ------------------------------------------------------------------ */
/* 绘制                                                                */
/* ------------------------------------------------------------------ */

static void App_DrawBars(void)
{
    static StatusBar_Info s_sb_prev;   /* 上一次真正画到屏上的状态栏内容 */
    static uint8_t        s_sb_valid;

    StatusBar_Info sb;
    ParamBar_Info  pb;

    /* 先清零，结构体填充字节才不会让 memcmp 误判 */
    (void)memset(&sb, 0, sizeof(sb));

    sb.channel      = s_st.measure_ch;
    sb.vdiv_mv1     = App_VdivMv(FRONTEND_CH1);
    sb.vdiv_mv2     = App_VdivMv(FRONTEND_CH2);
    sb.tdiv_us      = s_st.tdiv_us;
    sb.sample_rate  = s_st.sps;
    sb.range1       = (uint8_t)Frontend_GetRange(FRONTEND_CH1);
    sb.range2       = (uint8_t)Frontend_GetRange(FRONTEND_CH2);
    sb.running      = s_st.running;
    sb.rate_limited = s_st.rate_limited;
    sb.fft          = (s_st.show_mode == APP_SHOW_FFT) ? 1U : 0U;

    /* 状态栏内容在多数帧里不变，整条重画要 14 KB，值得跳过 */
    if ((s_sb_valid == 0U) || (memcmp(&s_sb_prev, &sb, sizeof(sb)) != 0))
    {
        StatusBar_Draw(&sb);
        s_sb_prev  = sb;
        s_sb_valid = 1U;
    }

    pb.channel   = s_st.measure_ch;
    /* 暂停(HOLD)时保留最后一帧的测量值：大多数示波器都是这样，
       波形冻结住、数值也跟着冻结，方便读数。只看是否采到过数据。
       两路的测量结果随同一帧发布，所以 HOLD 时按 KEY1 切通道，
       参数栏会立刻换成另一路在最后一帧上的结果，不会停在旧通道。 */
    if (s_view.seq != 0U)
    {
        const Measure_Result *m = &s_view.meas[(s_st.measure_ch == 1U) ? 0U : 1U];

        pb.valid     = m->valid;
        pb.freq_mhz  = m->freq_mhz;
        pb.period_us = m->period_us;
        pb.vpp_mv    = m->vpp_mv;
        pb.rms_mv    = m->rms_mv;
        pb.mean_mv   = m->mean_mv;
    }
    else
    {
        pb.valid     = 0U;
        pb.freq_mhz  = 0;
        pb.period_us = 0;
        pb.vpp_mv    = 0;
        pb.rms_mv    = 0;
        pb.mean_mv   = 0;
    }
    ParamBar_Draw(&pb);
}

/* 把 dB 谱线合成为一条 320 点"假波形"：
   dB(0..-60) -> y(22..211) -> mV（按 APP_FFT_VDIV 的换算反推）。
   复用 WaveView 的好处：网格、脏行、行指纹、触发电平线的隐藏
   全部沿用现成逻辑，频谱也能保持增量刷新的帧率。 */
static void App_BuildSpectrum(const Fft_Result *fft)
{
    uint16_t x;

    for (x = 0U; x < APP_CAP_LEN; x++)
    {
        uint16_t bin = (uint16_t)(((uint32_t)x * (uint32_t)fft->bins) / APP_CAP_LEN);
        int32_t  db  = (int32_t)fft->mag_db[bin];
        int32_t  h   = ((db + 60) * (int32_t)(VIEW_WAVE_H - 1U)) / 60;   /* 0..189 */
        int32_t  y   = (int32_t)(VIEW_WAVE_Y + VIEW_WAVE_H - 1U) - h;    /* 22..211 */

        s_spec[x] = ((int32_t)VIEW_WAVE_MID_Y - y) * 100;   /* y = 117 - mv/100 */
    }
}

static void App_DrawWave(void)
{
    WaveView_Data wd;

    /* 还没采到过数据（上电 HOLD 状态）：保持空白，不要画一条假的 0 V 直线 */
    if (s_view.seq == 0U)
    {
        return;
    }

    /* FFT 频谱：把谱线合成为一条"假波形"交给同一个 WaveView。
       只在出现新帧时重算 —— HOLD 时频谱随画面一起冻结。 */
    if (s_st.show_mode == APP_SHOW_FFT)
    {
        const int32_t *src   = (s_st.measure_ch == 2U) ? s_view.mv2 : s_view.mv1;
        uint8_t        is_ch1 = (s_st.measure_ch == 1U) ? 1U : 0U;

        if (s_view.seq != s_fft_seq)
        {
            Fft_Analyze(src, APP_CAP_LEN, &s_fft);
            s_fft_seq = s_view.seq;
        }
        App_BuildSpectrum(&s_fft);

        wd.ch1        = (is_ch1 != 0U) ? s_spec : 0;
        wd.ch2        = (is_ch1 != 0U) ? 0 : s_spec;
        wd.count      = APP_CAP_LEN;
        wd.vdiv_mv1   = APP_FFT_VDIV;
        wd.vdiv_mv2   = APP_FFT_VDIV;
        wd.vdiv_mv_trig = APP_FFT_VDIV;    /* 频谱里不画电平线，给个有效值防除零 */
        wd.show1      = is_ch1;
        wd.show2      = (uint8_t)((is_ch1 != 0U) ? 0U : 1U);
        wd.trig_mv    = 0;
        wd.trig_state = APP_TRIG_STATE_OFF;   /* 频谱模式下不画触发电平线 */
        WaveView_DrawTrace(&wd);
        return;
    }

    /* 网格/背景由 View_DrawFrame() 或 WaveView_Invalidate() 负责整片重画；
       这里只让 WaveView 增量刷新曲线经过的脏行。 */
    wd.ch1       = s_view.mv1;
    wd.ch2       = s_view.mv2;
    wd.count     = APP_CAP_LEN;
    wd.vdiv_mv1  = App_VdivMv(FRONTEND_CH1);
    wd.vdiv_mv2  = App_VdivMv(FRONTEND_CH2);
    wd.vdiv_mv_trig = App_TrigVdivMv();   /* 电平线按触发通道的档位换算 */
    wd.show1     = (s_st.show_mode != APP_SHOW_CH2) ? 1U : 0U;
    wd.show2     = (s_st.show_mode != APP_SHOW_CH1) ? 1U : 0U;
    wd.trig_mv   = s_st.trig_level_mv;
    /* 触发刚在菜单里关掉、但最后一帧是带触发采的：不能拿帧里的旧状态画标记，
       以当前设置为准（关掉后标记立即消失）。 */
    wd.trig_state = (s_st.trig_edge == (uint8_t)TRIGGER_EDGE_OFF)
                        ? APP_TRIG_STATE_OFF
                        : s_view.trig;
    WaveView_DrawTrace(&wd);
}

static void App_DrawMenu(void)
{
    ScopeMenu_Info mi;

    mi.sel          = s_st.menu_sel;
    mi.vdiv_mv1     = App_VdivMv(FRONTEND_CH1);
    mi.vdiv_mv2     = App_VdivMv(FRONTEND_CH2);
    mi.tdiv_us      = k_tdiv_us[s_st.tdiv_idx];
    mi.trig_src     = s_st.trig_src;
    mi.trig_edge    = (Trigger_Edge)s_st.trig_edge;
    mi.trig_level_mv = s_st.trig_level_mv;
    mi.measure_ch    = s_st.measure_ch;
    ScopeMenu_Draw(&mi);
}

/* ------------------------------------------------------------------ */
/* 输入                                                                */
/* ------------------------------------------------------------------ */

static uint8_t App_ClampStep(uint8_t idx, int16_t delta, uint8_t count)
{
    int16_t v = (int16_t)idx + delta;

    if (v < 0)
    {
        v = 0;
    }
    if (v >= (int16_t)count)
    {
        v = (int16_t)(count - 1U);
    }
    return (uint8_t)v;
}

/* ------------------------------------------------------------------ */
/* 自动设置（AUTO）：按一帧数据自动选 V/div 与 Time/div                 */
/* ------------------------------------------------------------------ */

/* 取一段 mV 数据的最小/最大值（只看当前显示窗口的 320 点） */
static void App_FrameRange(const int32_t *mv, int32_t *lo_out, int32_t *hi_out)
{
    int32_t  lo = mv[0];
    int32_t  hi = mv[0];
    uint16_t i;

    for (i = 1U; i < APP_CAP_LEN; i++)
    {
        if (mv[i] < lo)
        {
            lo = mv[i];
        }
        if (mv[i] > hi)
        {
            hi = mv[i];
        }
    }

    *lo_out = lo;
    *hi_out = hi;
}

/* 单路选档：在"放得下"的前提下选最灵敏的一档。
   放得下 = 峰峰值不超过 6 格（上下各留 2 格余量）
            且绝对峰值不超过 4.5 格（带直流偏置的信号不会顶出屏幕）。
   Vpp 与峰值都是前端换算出的真实输入 mV，与当前档位无关，
   所以只要数据本身没被量程削顶，一帧就能定档。 */
static uint8_t App_AutoPickVdiv(const int32_t *mv)
{
    int32_t lo = 0;
    int32_t hi = 0;
    uint8_t i;

    App_FrameRange(mv, &lo, &hi);

    for (i = 0U; i < APP_VDIV_COUNT; i++)
    {
        int32_t vdiv = (int32_t)k_vdiv_mv[i];
        int32_t vpp  = hi - lo;
        int32_t peak = (hi > -lo) ? hi : -lo;

        if ((vpp <= (6 * vdiv)) && ((peak * 2) <= (9 * vdiv)))
        {
            return i;
        }
    }

    return (uint8_t)(APP_VDIV_COUNT - 1U);   /* 都放不下时用最粗档 */
}

/* 两路各自选档：双路模式两路都算，单路模式只算显示的那一路，
   FFT 只算当前通道（频谱源就是它）。 */
static void App_AutoVdiv(const int32_t *mv1, const int32_t *mv2)
{
    uint8_t changed = 0U;
    uint8_t ch;

    for (ch = 0U; ch < 2U; ch++)
    {
        uint8_t idx;

        if ((s_st.show_mode == APP_SHOW_CH1) && (ch == 1U))
        {
            continue;
        }
        if ((s_st.show_mode == APP_SHOW_CH2) && (ch == 0U))
        {
            continue;
        }
        if ((s_st.show_mode == APP_SHOW_FFT) && (ch != (uint8_t)(s_st.measure_ch - 1U)))
        {
            continue;
        }

        idx = App_AutoPickVdiv((ch == 0U) ? mv1 : mv2);
        if (idx != s_st.vdiv_idx[ch])
        {
            s_st.vdiv_idx[ch] = idx;
            changed = 1U;
        }
    }

    if (changed != 0U)
    {
        App_ApplyRanges();   /* 跨过 1 V / 2 V 分界时同时切前端 S/W 量程 */
        App_ClampLevel();    /* ±5 格的电平范围随触发通道档位变化 */
    }
}

/* Time/div：屏幕上至少放 2 个完整周期，取满足条件里最快的档位。
   1-2-5 步进决定了结果落在 2~5 个周期之间（再粗一档就不到 2 个周期）。
   周期测不到（直流 / 幅度小于 60 mV）时保持当前时基不动。 */
static void App_AutoTdiv(const Measure_Result meas[2])
{
    int32_t period_us;
    uint8_t i;

    /* 参数栏正在测的那一路优先；测不到就看另一路 */
    period_us = meas[s_st.measure_ch - 1U].period_us;
    if (period_us <= 0)
    {
        period_us = meas[(s_st.measure_ch == 1U) ? 1U : 0U].period_us;
    }
    if (period_us <= 0)
    {
        return;
    }

    for (i = 0U; i < APP_TDIV_COUNT; i++)
    {
        /* 屏幕 8 格 = 8*Time/div；至少 2 个周期：8*tdiv >= 2*period */
        if (((int64_t)8 * (int64_t)k_tdiv_us[i]) >= ((int64_t)2 * (int64_t)period_us))
        {
            break;
        }
    }
    if (i >= APP_TDIV_COUNT)            /* 最慢档也放不下 2 个周期 */
    {
        i = (uint8_t)(APP_TDIV_COUNT - 1U);
    }

    if (i != s_st.tdiv_idx)
    {
        s_st.tdiv_idx = i;
        App_Timebase();
    }
}

/* 自动设置一次：只改 V/div 与 Time/div，触发源/边沿/电平都不动
   （电平的 AUTO 在采集路径里：找不到触发点时自动取信号中点）。
   菜单里按 KEY4 时用当前屏幕帧先算一遍；随后界面任务每拉到一帧新数据
   再算几遍（s_auto_pass），这样量程/时基已经换过之后的数据也能参与决策。 */
static void App_AutoSetup(const int32_t *mv1, const int32_t *mv2,
                          const Measure_Result meas[2])
{
    App_AutoVdiv(mv1, mv2);
    App_AutoTdiv(meas);
}

/* 返回 1 表示需要重画 */
static uint8_t App_HandleMain(int16_t d1, int16_t d2, uint8_t key)
{
    uint8_t redraw = 0U;

    if (key == 1U)                  /* KEY1：切换当前通道（测量 + 触发源） */
    {
        s_st.measure_ch = (s_st.measure_ch == 1U) ? 2U : 1U;
        s_st.trig_src   = s_st.measure_ch;   /* 选中的通道同时成为触发源 */
        App_ClampLevel();                    /* 触发通道换了，±5 格范围可能变 */
        redraw          = 1U;
    }
    else if (key == 2U)
    {
        s_st.menu     = 1U;
        s_st.menu_sel = 0U;
        redraw        = 1U;
    }
    else if (key == 3U)                 /* KEY3：显示模式循环（含 FFT） */
    {
        s_st.show_mode = (uint8_t)((s_st.show_mode + 1U) % 4U);
        redraw         = 1U;
    }
    else if (key == 4U)
    {
        s_st.running = (s_st.running == 0U) ? 1U : 0U;
        redraw       = 1U;
    }

    /* 编码器1：当前通道的垂直档位（两路各自独立，KEY1 决定调哪一路） */
    if (d1 != 0)
    {
        uint8_t ch = (uint8_t)(s_st.measure_ch - 1U);

        s_st.vdiv_idx[ch] = App_ClampStep(s_st.vdiv_idx[ch], d1, APP_VDIV_COUNT);
        App_ApplyRanges();
        App_ClampLevel();   /* 触发通道的 ±5 格范围可能跟着变 */
        redraw = 1U;
    }
    if (d2 != 0)
    {
        s_st.tdiv_idx = App_ClampStep(s_st.tdiv_idx, d2, APP_TDIV_COUNT);
        App_Timebase();
        redraw = 1U;
    }

    return redraw;
}

/* ------------------------------------------------------------------ */
/* 前端标定（逐通道、逐量程的两点法）                                   */
/* ------------------------------------------------------------------ */

/* 把当前步骤的提示画到波形区中央（会先把提示条所在的行清成背景色） */
static void App_CalDraw(void)
{
    char line[52];

    if (s_cal_step == APP_CAL_GND)
    {
        (void)snprintf(line, sizeof(line), "CAL CH%u/%s GND KEY4  K1:CH K2:ESC",
                       (unsigned)s_cal_ch, Frontend_RangeLetter(s_cal_ch));
    }
    else if (s_cal_step == APP_CAL_SPAN)
    {
        (void)snprintf(line, sizeof(line), "CAL CH%u/%s 3V3 KEY4  K1:CH K2:ESC",
                       (unsigned)s_cal_ch, Frontend_RangeLetter(s_cal_ch));
    }
    else
    {
        (void)snprintf(line, sizeof(line), "CAL CH%u/%s",
                       (unsigned)s_cal_ch, Frontend_RangeLetter(s_cal_ch));
    }

    /* 和采集任务共用总线锁：不能一边 DMA 采样一边刷 LCD（见问题 28）。
       进入标定后 running 已经清零，这里的等待最多等完手上那一帧。 */
    App_BusLock();
    WaveView_DrawMessage(line);
    App_BusUnlock();
}

/* 连采 64 次单次转换求平均。返回 1 = 成功。
   调用前不能持有总线锁（函数内部会取锁，保证和采集任务不抢 ADC）。 */
static uint8_t App_CalSample(uint8_t channel, uint16_t *raw_out)
{
    uint32_t sum = 0U;
    uint32_t cnt = 0U;
    uint16_t r1  = 0U;
    uint16_t r2  = 0U;
    uint32_t i;

    App_BusLock();
    for (i = 0U; i < APP_CAL_SAMPLES; i++)
    {
        if (ADC_Drv_ReadOnce(&r1, &r2) != ADC_DRV_OK)
        {
            break;
        }

        sum += (channel == FRONTEND_CH2) ? (uint32_t)r2 : (uint32_t)r1;
        cnt++;
    }
    App_BusUnlock();

    if (cnt == 0U)
    {
        return 0U;
    }

    *raw_out = (uint16_t)(sum / cnt);
    return 1U;
}

/* 结束标定：恢复采集状态，回到菜单，并把屏幕整片标记为待重画 */
static void App_CalFinish(void)
{
    s_cal_step     = APP_CAL_IDLE;
    s_st.running   = s_cal_resume_run;
    s_st.menu      = 1U;            /* 标定是从菜单进来的，结束后回菜单 */
    WaveView_Invalidate();          /* 提示条盖过波形区，退出后整片重画 */
    s_draw_pending = 1U;
}

/* 开始标定：暂停采集，默认标定主界面 KEY1 选中的通道 */
static void App_CalStart(void)
{
    s_cal_resume_run = s_st.running;
    s_st.running     = 0U;          /* 单次转换和 DMA 不能同时占用 ADC */
    s_cal_ch         = s_st.measure_ch;
    s_st.trig_src    = s_cal_ch;    /* 标定哪个通道，哪个通道就是触发源 */
    App_ClampLevel();               /* 触发通道换了，±5 格范围可能变 */
    s_cal_step       = APP_CAL_GND;
    App_CalDraw();
}

/* 标定模式下的按键处理。编码器在标定模式下不参与。 */
static void App_CalHandle(uint8_t key)
{
    uint16_t raw = 0U;

    if (key == 1U)                  /* KEY1：换一个通道，重新从 GND 开始 */
    {
        s_cal_ch = (s_cal_ch == FRONTEND_CH1) ? FRONTEND_CH2 : FRONTEND_CH1;
        s_st.measure_ch = s_cal_ch; /* 参数栏也跟着切到正在标定的通道 */
        s_st.trig_src   = s_cal_ch; /* 触发源跟着走，标定完就看这一路 */
        App_ClampLevel();           /* 触发通道换了，±5 格范围可能变 */
        s_cal_step = APP_CAL_GND;
        App_CalDraw();
        return;
    }

    if (key == 2U)                  /* KEY2：取消 */
    {
        App_CalFinish();
        return;
    }

    if (key != 4U)
    {
        return;
    }

    if (s_cal_step == APP_CAL_GND)
    {
        if (App_CalSample(s_cal_ch, &raw) == 0U)
        {
            WaveView_DrawMessage("CAL FAIL: ADC BUSY  KEY4");
            return;
        }

        s_cal_zero_raw = raw;
        s_cal_step     = APP_CAL_SPAN;
        App_CalDraw();
    }
    else if (s_cal_step == APP_CAL_SPAN)
    {
        int32_t span;

        if (App_CalSample(s_cal_ch, &raw) == 0U)
        {
            WaveView_DrawMessage("CAL FAIL: ADC BUSY  KEY4");
            return;
        }

        span = (int32_t)raw - (int32_t)s_cal_zero_raw;

        if (Frontend_Calibrate(s_cal_ch, s_cal_zero_raw, raw) == 0U)
        {
            /* 码值差不对：要么没接 3.3 V，要么跳线位置和 V/div 不一致 */
            WaveView_DrawMessage("CAL FAIL: 3V3/RANGE? KEY4");
            return;     /* 停在 SPAN 步骤，可以重接后重按 KEY4 */
        }

        (void)printf("CAL CH%u/%s zero=%u span=%u diff=%ld\r\n",
                     (unsigned)s_cal_ch, Frontend_RangeLetter(s_cal_ch),
                     (unsigned)s_cal_zero_raw, (unsigned)raw, (long)span);

        WaveView_DrawMessage("CAL SAVING...");
        if (Frontend_CalSave() == 0U)
        {
            WaveView_DrawMessage("CAL OK  SAVE FAIL");
        }
        else
        {
            WaveView_DrawMessage("CAL OK  SAVED");
        }

        s_cal_step     = APP_CAL_DONE;
        s_cal_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(APP_CAL_DONE_MS);
    }
    else
    {
        /* DONE：等 UiStep 计时退出，忽略按键 */
    }
}

static uint8_t App_HandleMenu(int16_t d1, int16_t d2, uint8_t key)
{
    uint8_t redraw = 0U;

    /* 用户自己动了按键/编码器：取消 AUTO 还没跑完的后续遍数，
       免得过两帧又把他刚调好的档位改掉。Auto 行按 KEY4 会重新置位。 */
    if ((d1 != 0) || (d2 != 0) || (key != 0U))
    {
        s_auto_pass = 0U;
    }

    if (key == 2U)
    {
        s_st.menu = 0U;
        WaveView_Invalidate();       /* 弹窗盖过波形区，退出后必须整片重画 */
        return 1U;
    }

    /* KEY1：任何一行都切换“当前通道”（参数栏测量 + 触发源 + V/div/Cal 目标） */
    if (key == 1U)
    {
        s_st.measure_ch = (s_st.measure_ch == 1U) ? 2U : 1U;
        s_st.trig_src   = s_st.measure_ch;   /* 当前通道同时是触发源 */
        App_ClampLevel();                    /* 触发通道换了，±5 格范围可能变 */
        redraw          = 1U;
    }

    /* 标定行：KEY4 启动两点标定（标定哪一路由上面的 KEY1 决定） */
    if (s_st.menu_sel == SCOPE_MENU_ROW_CAL)
    {
        if (key == 4U)
        {
            App_CalStart();
            return 1U;
        }
    }

    /* 自动设置行：KEY4 用当前屏幕帧做一遍 AUTO，并安排后续新帧再判几遍 */
    if (s_st.menu_sel == SCOPE_MENU_ROW_AUTO)
    {
        if (key == 4U)
        {
            if (s_view.seq != 0U)
            {
                App_AutoSetup(s_view.mv1, s_view.mv2, s_view.meas);
                s_auto_pass = 2U;
            }
            return 1U;
        }
    }

    if (d1 != 0)
    {
        int16_t sel = (int16_t)s_st.menu_sel + d1;

        while (sel < 0)
        {
            sel += (int16_t)SCOPE_MENU_ROWS;
        }
        s_st.menu_sel = (uint8_t)(sel % (int16_t)SCOPE_MENU_ROWS);
        redraw = 1U;
    }

    if (d2 != 0)
    {
        switch (s_st.menu_sel)
        {
            case SCOPE_MENU_ROW_VDIV:
            {
                uint8_t ch = (uint8_t)(s_st.measure_ch - 1U);

                s_st.vdiv_idx[ch] = App_ClampStep(s_st.vdiv_idx[ch], d2,
                                                  APP_VDIV_COUNT);
                App_ApplyRanges();
                App_ClampLevel();
                break;
            }

            case SCOPE_MENU_ROW_TIM:
                s_st.tdiv_idx = App_ClampStep(s_st.tdiv_idx, d2, APP_TDIV_COUNT);
                App_Timebase();
                break;

            case SCOPE_MENU_ROW_TRIG_SRC:
                s_st.trig_src = (s_st.trig_src == 1U) ? 2U : 1U;
                App_ClampLevel();   /* 触发通道换了，±5 格范围可能变 */
                break;

            case SCOPE_MENU_ROW_TRIG_EDGE:
                App_StepEdge(d2);
                break;

            case SCOPE_MENU_ROW_TRIG_LEVEL:
                /* 步进 = 触发通道的 1/10 格 */
                s_st.trig_level_mv += (int32_t)d2 *
                                      ((int32_t)App_TrigVdivMv() / 10);
                App_ClampLevel();
                break;

            default:
                break;
        }
        redraw = 1U;
    }

    return redraw;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void App_Scope_Init(void)
{
    s_st.measure_ch   = 1U;
    s_st.show_mode    = APP_SHOW_BOTH;
    s_st.vdiv_idx[0]  = 2U;      /* CH1：500 mV/div */
    s_st.vdiv_idx[1]  = 2U;      /* CH2：500 mV/div */
    s_st.tdiv_idx     = 5U;      /* 1.00 ms/div -> 40 kSa/s */
    s_st.running      = 0U;      /* 上电先停在 HOLD，等用户按 KEY4 再开始测量 */
    s_st.rate_limited = 0U;
    s_st.menu         = 0U;
    s_st.menu_sel     = 0U;
    s_st.trig_src     = 1U;                          /* 默认在 CH1 上触发 */
    s_st.trig_edge    = (uint8_t)TRIGGER_EDGE_RISING;/* 默认上升沿 */
    s_st.trig_level_mv = 0;                          /* 默认以 0 V 为触发电平 */

    (void)memset(&s_frame, 0, sizeof(s_frame));
    (void)memset(&s_view, 0, sizeof(s_view));
    s_have_frame = 0U;

    /* 帧交接互斥量：调度器启动前创建也是合法的 */
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);

    /* 屏刷 / 采集互斥量：让 ADC 采样与 LCD SPI 突发不同时发生 */
    s_bus_lock = xSemaphoreCreateMutex();
    configASSERT(s_bus_lock != NULL);

    Lcd_Init();
    Lcd_Backlight(1U);

    EncoderDrv_Reset();

    App_Timebase();
    App_ApplyRanges();

    View_DrawFrame();

    /* 上电停在 HOLD：提示用户按运行键。
       这行字只需画一次，第一次真正采集重画波形区时会被整片覆盖掉。 */
    WaveView_DrawMessage("PRESS KEY4 TO RUN");

    App_DrawBars();
}

/* ------------------------------------------------------------------ */
/* 界面重绘（带总线锁超时）                                             */
/* ------------------------------------------------------------------ */

/* 取总线锁最长等 20 ms：40 kSa/s 下一帧采集约 16 ms + 处理约 2 ms，
   采集任务放手后会显式让出 1 tick，所以界面任务几乎总能在下一轮拿到锁；
   慢时基下一帧要几百毫秒，等不到就先记着，下一轮再试。 */
#define APP_UI_BUS_WAIT_MS  20U

/* 画一帧波形 + 上下条带（菜单打开时连弹窗一起画）。
   返回 1 = 已画完并放锁；0 = 锁被采集任务占着，本帧先不画。 */
static uint8_t App_UiDrawFrame(void)
{
    if (App_BusLockWait(APP_UI_BUS_WAIT_MS) == 0U)
    {
        return 0U;
    }

    App_DrawWave();
    App_DrawBars();

    /* 本帧刚按 KEY2 进入菜单：弹窗也要在这一帧画出来。
       否则要等到菜单里产生一次输入（转编码器）才会刷新，
       表现为"按 KEY2 没反应，转一下编码器菜单才出现"。 */
    if (s_st.menu != 0U)
    {
        App_DrawMenu();
    }

    App_BusUnlock();

    return 1U;
}

uint8_t App_Scope_AcqStep(void)
{
    uint32_t       sps;
    uint16_t       win_start;
    uint8_t        trig_state;
    Measure_Result meas[2];

    /* HOLD：不采集，直接把 CPU 让出去 */
    if (s_st.running == 0U)
    {
        return 0U;
    }

    /* 参数各自是原子标量，先取到本地，保证一帧内用的是同一组值 */
    sps = s_st.sps;

    /* 采集期间不刷屏：避免 SPI 电流尖峰耦合进采样窗口（见 APP_SERIALIZE_ACQ_DRAW） */
    App_BusLock();
    (void)App_Capture(sps, &win_start, &trig_state);
    App_BusUnlock();

    /* 波形和测量必须用同一份缓冲（同一个显示窗口）。采集判定失败时缓冲区里
       也可能已经搬进了大半帧（波形区照样在画它），所以测量无条件跟着走，
       否则会出现"波形在动、RMS/DC 一动不动"。
       两路都测：HOLD 时按 KEY1 切通道，要能立刻显示另一路的最后一帧结果。 */
    Measure_Analyze(&s_mv1[win_start], APP_CAP_LEN, sps, &meas[0]);
    Measure_Analyze(&s_mv2[win_start], APP_CAP_LEN, sps, &meas[1]);

    App_PublishFrame(meas, sps, win_start, trig_state);

    return 1U;
}

uint8_t App_Scope_UiStep(void)
{
    int16_t d1;
    int16_t d2;
    uint8_t key;
    uint8_t redraw = 0U;

    d1  = EncoderDrv_ReadDelta(ENCODER_DRV_1);
    d2  = EncoderDrv_ReadDelta(ENCODER_DRV_2);
    key = Key_GetNum();

    /* 标定模式：只处理 KEY1（换通道）/ KEY2（取消）/ KEY4（采当前点），
       编码器不参与；提示直接画在波形区中央，不走常规重绘。 */
    if (s_cal_step != APP_CAL_IDLE)
    {
        if (s_cal_step == APP_CAL_DONE)
        {
            if ((xTaskGetTickCount() >= s_cal_deadline) || (key == 2U) || (key == 4U))
            {
                App_CalFinish();
            }
        }
        else
        {
            App_CalHandle(key);
        }
        return 1U;
    }

    if (s_st.menu != 0U)
    {
        redraw = App_HandleMenu(d1, d2, key);

        /* 刚按 KEY4 启动标定：提示已经画好，本帧不要再用弹窗盖上去 */
        if (s_cal_step != APP_CAL_IDLE)
        {
            return 1U;
        }

        /* AUTO 的后续几遍：等界面任务拉到新帧再算。第一遍用的是按 KEY4
           之前的帧，可能是在错误量程/时基下测的（比如小量程削顶的信号），
           换过档位后的新帧再判一次才能收敛。没有新帧就下一轮再看。 */
        if ((s_st.menu != 0U) && (s_auto_pass != 0U))
        {
            if (App_PullFrame() != 0U)
            {
                s_auto_pass--;
                App_AutoSetup(s_view.mv1, s_view.mv2, s_view.meas);
                redraw = 1U;
            }
        }

        if (s_st.menu != 0U)
        {
            /* 仍在菜单里：重画波形区 + 弹窗 + 上下条带 */
            if (redraw != 0U)
            {
                (void)App_PullFrame();
                s_draw_pending = 1U;
            }

            if ((s_draw_pending != 0U) && (App_UiDrawFrame() != 0U))
            {
                s_draw_pending = 0U;
            }
            return redraw;
        }

        /* 刚刚按 KEY2 退出菜单。
           这里必须把 menu 已置 0 的状态当作主界面处理：
           本帧就完整重画主界面，且**不再调用 App_DrawMenu()**，
           否则弹窗会在退出这一帧被重新画上去，看起来"只是把窗口擦了、
           其实还留在菜单状态"。 */
        redraw = 1U;
    }
    else
    {
        redraw = App_HandleMain(d1, d2, key);
    }

    /* 采集任务发布了更新的帧就重画；HOLD 时没有新帧，画面保持冻结 */
    if (App_PullFrame() != 0U)
    {
        redraw = 1U;
    }

    /* 记下"欠一帧"：这次拿不到总线锁也留到下一轮画，不把按键输入一起卡住 */
    if (redraw != 0U)
    {
        s_draw_pending = 1U;
    }

    if ((s_draw_pending != 0U) && (App_UiDrawFrame() != 0U))
    {
        s_draw_pending = 0U;
    }

    return redraw;
}
