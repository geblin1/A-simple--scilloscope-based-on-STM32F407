/**
  * @file    App_Scope.c
  * @brief   示波器应用层：输入处理 + 采集 + 状态 + 重绘
  *
  * 界面按键约定（MAIN 模式）
  * ------------------------
  *   KEY1 切换参数栏测量的通道（CH1 / CH2）
  *   KEY2 进入设置菜单
  *   KEY3 切换显示通道（全显示 / 只看 CH1 / 只看 CH2）
  *   KEY4 运行 / 停止
  *   编码器1  调 V/div（CH1 与 CH2 一起缩放）
  *   编码器2  调 Time/div
  *
  * 菜单模式
  * --------
  *   编码器1 上下移动选中行，编码器2 改数值，KEY2 退出
  */

#include "App_Scope.h"

#include <stdio.h>
#include <string.h>

#include "ADC_Drv.h"
#include "EncoderDrv.h"
#include "Frontend.h"
#include "Key.h"
#include "Lcd.h"
#include "Measure.h"
#include "ScopeMenu.h"
#include "ScopeView.h"

/* 横屏一屏 320 列，一列一个采样点 */
#define APP_CAP_LEN         320U
#define APP_CAP_TIMEOUT_MS  60U

#define APP_VDIV_COUNT      7U
#define APP_TDIV_COUNT      12U

/* V/div 与前端量程的对应关系
   --------------------------
   软件检测不到跳线乙，所以用 V/div 档位间接指定量程：
     前 4 档（100 / 200 / 500 / 1000 mV/div）-> 小量程，跳线乙插 MID
     后 3 档（2000 / 5000 / 10000 mV/div）   -> 大量程，跳线乙不插
   状态栏右下角会显示 S / W，提示用户该把跳线放哪一档。
   跨过这个分界时波形会突然变形，那是正常的——说明跳线位置和新档位对不上。 */
#define APP_VDIV_SMALL_MAX  3U

/* 显示通道组合 */
#define APP_SHOW_BOTH       0U
#define APP_SHOW_CH1        1U
#define APP_SHOW_CH2        2U

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
    uint8_t  vdiv_idx;      /* 双通道共用同一个垂直档位 */
    uint8_t  tdiv_idx;
    uint8_t  running;
    uint8_t  rate_limited;
    uint8_t  menu;
    uint8_t  menu_sel;
    uint32_t sps;
    uint32_t tdiv_us;
} App_State;

static App_State       s_st;
static uint32_t        s_raw[APP_CAP_LEN];
static int32_t         s_mv1[APP_CAP_LEN];   /* 已标定的输入电压（mV，带符号） */
static int32_t         s_mv2[APP_CAP_LEN];
static Measure_Result  s_meas;
static uint8_t         s_have_data;

/* ------------------------------------------------------------------ */
/* 前端量程（由 V/div 档位决定，见 k_vdiv_mv 上方说明）                */
/* ------------------------------------------------------------------ */

static void App_ApplyRange(void)
{
    Frontend_SetRange((s_st.vdiv_idx <= APP_VDIV_SMALL_MAX)
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
/* 采集                                                                */
/* ------------------------------------------------------------------ */

static uint8_t App_Capture(void)
{
    uint16_t i;
    uint8_t  status;

    status = (uint8_t)ADC_Drv_CaptureStart(s_raw, APP_CAP_LEN);
    if (status == (uint8_t)ADC_DRV_OK)
    {
        status = (uint8_t)ADC_Drv_CaptureWait(APP_CAP_TIMEOUT_MS);
        (void)ADC_Drv_CaptureStop();
    }

    /* 不管成功还是超时都拆一遍：超时时缓冲区里可能是上一次的旧数据，
       但至少不会让画面上出现全 0 的假直线。 */
    for (i = 0U; i < APP_CAP_LEN; i++)
    {
        s_mv1[i] = Frontend_RawToMv(ADC_Drv_WordCh1(s_raw[i]));
        s_mv2[i] = Frontend_RawToMv(ADC_Drv_WordCh2(s_raw[i]));
    }

    return (status == (uint8_t)ADC_DRV_OK) ? 1U : 0U;
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
    sb.vdiv_mv      = k_vdiv_mv[s_st.vdiv_idx];
    sb.tdiv_us      = s_st.tdiv_us;
    sb.sample_rate  = s_st.sps;
    sb.ac_dc        = 0U;      /* 耦合由硬件跳线决定，这里固定显示 DC */
    sb.range        = (uint8_t)Frontend_GetRange();
    sb.running      = s_st.running;
    sb.rate_limited = s_st.rate_limited;

    /* 状态栏内容在多数帧里不变，整条重画要 14 KB，值得跳过 */
    if ((s_sb_valid == 0U) || (memcmp(&s_sb_prev, &sb, sizeof(sb)) != 0))
    {
        StatusBar_Draw(&sb);
        s_sb_prev  = sb;
        s_sb_valid = 1U;
    }

    pb.channel   = s_st.measure_ch;
    /* 暂停(HOLD)时保留最后一帧的测量值：大多数示波器都是这样，
       波形冻结住、数值也跟着冻结，方便读数。只看是否采到过数据。 */
    if (s_have_data != 0U)
    {
        pb.valid     = s_meas.valid;
        pb.freq_mhz  = s_meas.freq_mhz;
        pb.period_us = s_meas.period_us;
        pb.vpp_mv    = s_meas.vpp_mv;
        pb.rms_mv    = s_meas.rms_mv;
        pb.mean_mv   = s_meas.mean_mv;
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

static void App_DrawWave(void)
{
    WaveView_Data wd;

    /* 还没采到过数据（上电 HOLD 状态）：保持空白，不要画一条假的 0 V 直线 */
    if (s_have_data == 0U)
    {
        return;
    }

    /* 网格/背景由 View_DrawFrame() 或 WaveView_Invalidate() 负责整片重画；
       这里只让 WaveView 增量刷新曲线经过的脏行。 */
    wd.ch1       = s_mv1;
    wd.ch2       = s_mv2;
    wd.count     = APP_CAP_LEN;
    wd.vdiv_mv1  = k_vdiv_mv[s_st.vdiv_idx];
    wd.vdiv_mv2  = k_vdiv_mv[s_st.vdiv_idx];
    wd.show1     = (s_st.show_mode != APP_SHOW_CH2) ? 1U : 0U;
    wd.show2     = (s_st.show_mode != APP_SHOW_CH1) ? 1U : 0U;
    WaveView_DrawTrace(&wd);
}

static void App_DrawMenu(void)
{
    ScopeMenu_Info mi;

    mi.sel       = s_st.menu_sel;
    mi.vdiv_mv   = k_vdiv_mv[s_st.vdiv_idx];
    mi.tdiv_us   = k_tdiv_us[s_st.tdiv_idx];
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

/* 返回 1 表示需要重画 */
static uint8_t App_HandleMain(int16_t d1, int16_t d2, uint8_t key)
{
    uint8_t redraw = 0U;

    if (key == 1U)
    {
        s_st.measure_ch = (s_st.measure_ch == 1U) ? 2U : 1U;
        redraw = 1U;
    }
    else if (key == 2U)
    {
        s_st.menu     = 1U;
        s_st.menu_sel = 0U;
        redraw        = 1U;
    }
    else if (key == 3U)
    {
        s_st.show_mode = (uint8_t)((s_st.show_mode + 1U) % 3U);
        redraw         = 1U;
    }
    else if (key == 4U)
    {
        s_st.running = (s_st.running == 0U) ? 1U : 0U;
        redraw       = 1U;
    }

    /* 编码器1：垂直档位。双通道共用同一个 V/div，
       这样两条波形用的是同一套电压-像素换算，不会出现"同屏却不同分辨率"。 */
    if (d1 != 0)
    {
        s_st.vdiv_idx = App_ClampStep(s_st.vdiv_idx, d1, APP_VDIV_COUNT);
        App_ApplyRange();
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

static uint8_t App_HandleMenu(int16_t d1, int16_t d2, uint8_t key)
{
    uint8_t redraw = 0U;

    if (key == 2U)
    {
        s_st.menu = 0U;
        WaveView_Invalidate();       /* 弹窗盖过波形区，退出后必须整片重画 */
        return 1U;
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
                s_st.vdiv_idx = App_ClampStep(s_st.vdiv_idx, d2, APP_VDIV_COUNT);
                App_ApplyRange();
                break;
            default:
                s_st.tdiv_idx = App_ClampStep(s_st.tdiv_idx, d2, APP_TDIV_COUNT);
                App_Timebase();
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
    s_st.vdiv_idx     = 2U;      /* 500 mV/div（双通道共用） */
    s_st.tdiv_idx     = 5U;      /* 1.00 ms/div -> 40 kSa/s */
    s_st.running      = 0U;      /* 上电先停在 HOLD，等用户按 KEY4 再开始测量 */
    s_st.rate_limited = 0U;
    s_st.menu         = 0U;
    s_st.menu_sel     = 0U;
    s_have_data       = 0U;

    Lcd_Init();
    Lcd_Backlight(1U);

    EncoderDrv_Reset();
    App_Timebase();
    App_ApplyRange();

    View_DrawFrame();

    /* 上电停在 HOLD：提示用户按运行键。
       这行字只需画一次，第一次真正采集重画波形区时会被整片覆盖掉。 */
    WaveView_DrawMessage("PRESS KEY4 TO RUN");

    App_DrawBars();
}

void App_Scope_Loop(void)
{
    int16_t  d1;
    int16_t  d2;
    uint8_t  key;
    uint8_t  redraw = 0U;

    d1  = EncoderDrv_ReadDelta(ENCODER_DRV_1);
    d2  = EncoderDrv_ReadDelta(ENCODER_DRV_2);
    key = Key_GetNum();

    if (s_st.menu != 0U)
    {
        redraw = App_HandleMenu(d1, d2, key);

        if (s_st.menu != 0U)
        {
            /* 仍在菜单里：重画波形区 + 弹窗 + 上下条带 */
            if (redraw != 0U)
            {
                App_DrawWave();
                App_DrawBars();
                App_DrawMenu();
            }
            return;
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

    if (s_st.running != 0U)
    {
        const int32_t *src = (s_st.measure_ch == 1U) ? s_mv1 : s_mv2;

        (void)App_Capture();

        /* 波形和测量必须用同一份缓冲。采集判定失败时缓冲区里也可能已经
           搬进了大半帧（波形区照样在画它），所以测量无条件跟着走，
           否则会出现"波形在动、RMS/DC 一动不动"。 */
        Measure_Analyze(src, APP_CAP_LEN, s_st.sps, &s_meas);
        s_have_data = 1U;

        /* 采集失败也要刷新：否则画面会停在上一帧，
           看起来就像"数据一动不动"或者"程序卡死了"。 */
        redraw = 1U;
    }

    if (redraw != 0U)
    {
        App_DrawWave();
        App_DrawBars();

        /* 本帧刚按 KEY2 进入菜单：弹窗也要在这一帧画出来。
           否则要等到菜单里产生一次输入（转编码器）才会刷新，
           表现为"按 KEY2 没反应，转一下编码器菜单才出现"。 */
        if (s_st.menu != 0U)
        {
            App_DrawMenu();
        }
    }
}
