/**
  * @file    WaveView.c
  * @brief   波形区绘制（320 x 190，8 列 x 10 行网格）—— 增量刷新
  *
  * 坐标换算
  * --------
  *   ADC 码值 -> 电压： v_mv = raw * 3300 / 4095
  *   电压 -> 像素   ： y = 中心线 - v_mv * 19 / vdiv_mv
  *                     （0 V 落在中轴线上：正电压向上、负电压向下，与常规示波器一致）
  *
  * 刷新策略（性能关键）
  * --------------------
  * 旧实现每帧都整片重画 320x190 的背景与网格（121.6 KB），再对 320 列各做一次
  * 零碎窗口写。SPI 只有 10.5 Mbit/s（约 1.31 MB/s），光背景就 90 ms 以上。
  *
  * 现在改成增量：
  *   1) 网格与背景只在 View_DrawFrame() 或显式 invalidate 之后整片画一次；
  *   2) 每帧只算出"新曲线 ∪ 旧曲线"覆盖到的行（脏行）；
  *   3) 每个脏行先把 320 个像素在行缓冲里合成好（模板 + 两路曲线），
  *      再用一次窗口一次性推出去。
  * 于是"320 次零碎窗口 + 121.6 KB"变成"脏行数 次整行窗口 + 脏行数 x 640 B"。
  * 典型波形只占 40~60 行，数据量约 30 KB，帧率提升 3 倍以上。
  */

#include "ScopeView.h"
#include <string.h>

#define WAVE_DIRTY_BYTES    ((VIEW_WAVE_H + 7U) / 8U)

/* ---------------- 增量刷新状态 ---------------- */
static uint16_t s_row_normal[VIEW_W];             /* 普通行模板（背景 + 竖向网格线） */
static uint16_t s_row_buf[VIEW_W];                /* 行合成缓冲 */
static int16_t  s_prev_y1[VIEW_W];                /* 上一帧两路曲线的 y */
static int16_t  s_prev_y2[VIEW_W];
static int16_t  s_cur_y1[VIEW_W];                 /* 本帧两路曲线的 y */
static int16_t  s_cur_y2[VIEW_W];
static uint8_t  s_dirty[WAVE_DIRTY_BYTES];
static uint32_t s_row_hash[VIEW_WAVE_H];          /* 每行上次真正推出去时的指纹 */
static uint8_t  s_row_ready;                      /* 行模板是否已生成 */
static uint8_t  s_valid;                          /* 1 = 屏幕与 s_prev_y 一致 */
static int16_t  s_prev_trig_y;                    /* 上一帧触发电平线所在行（trig_state=0 时无效） */
static uint8_t  s_prev_trig_state;

/* ---------------- 坐标换算 ---------------- */
static int16_t Wave_ClampY(int32_t y)
{
    if (y < (int32_t)VIEW_WAVE_Y)
    {
        return (int16_t)VIEW_WAVE_Y;
    }
    if (y > (int32_t)(VIEW_WAVE_Y + VIEW_WAVE_H - 1U))
    {
        return (int16_t)(VIEW_WAVE_Y + VIEW_WAVE_H - 1U);
    }
    return (int16_t)y;
}

/* 输入电压（mV，带符号）-> 波形区 y。
   0 V 固定画在中轴线上（示波器常规习惯），负电压自然落在中轴线以下。
   扣偏置零点、补衰减这些标定动作已经在 App 层由 Frontend 完成，View 不参与。 */
static int16_t Wave_SampleToY(int32_t mv, uint16_t vdiv_mv)
{
    if (vdiv_mv == 0U)
    {
        return VIEW_WAVE_MID_Y;
    }

    return Wave_ClampY((int32_t)VIEW_WAVE_MID_Y -
                       ((mv * VIEW_PX_PER_VDIV) / (int32_t)vdiv_mv));
}

/* ---------------- 行模板 / 脏行标记 ---------------- */
static void Wave_BuildRowTemplate(void)
{
    uint16_t x;
    uint16_t c;

    for (x = 0U; x < VIEW_W; x++)
    {
        c = VIEW_COLOR_BG;

        if ((x == 0U) || (x == (uint16_t)(VIEW_W - 1U)))
        {
            c = VIEW_COLOR_GRID;                  /* 左右边框 */
        }
        else if ((x % VIEW_CELL_W) == 0U)
        {
            c = (x == (uint16_t)(VIEW_W / 2U)) ? VIEW_COLOR_AXIS : VIEW_COLOR_GRID;
        }
        else
        {
            /* 普通背景 */
        }
        s_row_normal[x] = c;
    }

    s_row_ready = 1U;
}

/* 整行同色的行（上下边框、横向网格线、中心轴）返回 1 并给出颜色 */
static uint8_t Wave_FullRowColor(uint16_t y, uint16_t *color)
{
    uint16_t i = (uint16_t)(y - VIEW_WAVE_Y);

    if (y == (uint16_t)VIEW_WAVE_MID_Y)
    {
        *color = VIEW_COLOR_AXIS;
        return 1U;
    }
    if ((i == 0U) || (i == (uint16_t)(VIEW_WAVE_H - 1U)) ||
        ((i % VIEW_CELL_H) == 0U))
    {
        *color = VIEW_COLOR_GRID;
        return 1U;
    }
    return 0U;
}

static void Wave_DirtyClear(void)
{
    (void)memset(s_dirty, 0, sizeof(s_dirty));
}

static void Wave_DirtyAll(void)
{
    (void)memset(s_dirty, 0xFF, sizeof(s_dirty));
}

static void Wave_DirtyRow(uint16_t row)
{
    s_dirty[row >> 3] |= (uint8_t)(1U << (row & 7U));
}

static uint8_t Wave_IsDirty(uint16_t row)
{
    return (uint8_t)((s_dirty[row >> 3] >> (row & 7U)) & 1U);
}

/* 把 [lo, hi] 这段 y（可能超出边界）涉及的行标脏 */
static void Wave_DirtyRange(int32_t lo, int32_t hi)
{
    int32_t row;
    int32_t last;

    if (lo > hi)
    {
        int32_t t = lo;
        lo = hi;
        hi = t;
    }
    if (lo < (int32_t)VIEW_WAVE_Y)
    {
        lo = (int32_t)VIEW_WAVE_Y;
    }
    if (hi > (int32_t)(VIEW_WAVE_Y + VIEW_WAVE_H - 1U))
    {
        hi = (int32_t)(VIEW_WAVE_Y + VIEW_WAVE_H - 1U);
    }
    if (lo > hi)
    {
        return;
    }

    /* 逐行标脏代价很低（最多 190 次），换来调用点逻辑简单 */
    row  = lo;
    last = hi;
    while (row <= last)
    {
        Wave_DirtyRow((uint16_t)(row - (int32_t)VIEW_WAVE_Y));
        row++;
    }
}

/* ---------------- 网格（整片） ---------------- */
void WaveView_DrawGrid(void)
{
    uint16_t i;
    uint16_t x;
    uint16_t y;

    /* 背景 */
    Lcd_FillRect(0U, VIEW_WAVE_Y, VIEW_W, VIEW_WAVE_H, VIEW_COLOR_BG);

    /* 竖线（每格 40 px），中间那条用轴线色 */
    for (i = 1U; i < VIEW_GRID_COLS; i++)
    {
        x = (uint16_t)(i * VIEW_CELL_W);
        Lcd_DrawVLine(x, VIEW_WAVE_Y, VIEW_WAVE_H,
                      (i == (VIEW_GRID_COLS / 2U)) ? VIEW_COLOR_AXIS : VIEW_COLOR_GRID);
    }

    /* 横线（每格 19 px），中间那条用轴线色 */
    for (i = 1U; i < VIEW_GRID_ROWS; i++)
    {
        y = (uint16_t)(VIEW_WAVE_Y + (i * VIEW_CELL_H));
        Lcd_DrawHLine(0U, y, VIEW_W, (i == (VIEW_GRID_ROWS / 2U)) ? VIEW_COLOR_AXIS
                                                                  : VIEW_COLOR_GRID);
    }

    /* 边框 */
    Lcd_DrawRect(0U, VIEW_WAVE_Y, VIEW_W, VIEW_WAVE_H, VIEW_COLOR_GRID);

    /* 屏幕上现在只有网格，没有任何曲线 */
    s_valid = 0U;
}

void WaveView_Invalidate(void)
{
    s_valid = 0U;
}

/* ---------------- 曲线增量绘制 ---------------- */

/* 把一路曲线"旧位置 ∪ 新位置"涉及的行标脏 */
static void Wave_MarkChannel(const int16_t *prev, const int16_t *cur, uint16_t count)
{
    uint16_t i;
    int32_t  lo;
    int32_t  hi;
    int16_t  a;
    int16_t  b;

    for (i = 0U; i < count; i++)
    {
        a = (i == 0U) ? cur[0] : cur[i - 1U];
        b = cur[i];
        lo = (a < b) ? (int32_t)a : (int32_t)b;
        hi = (a > b) ? (int32_t)a : (int32_t)b;

        a = (i == 0U) ? prev[0] : prev[i - 1U];
        b = prev[i];
        if ((int32_t)a < lo) { lo = (int32_t)a; }
        if ((int32_t)b < lo) { lo = (int32_t)b; }
        if ((int32_t)a > hi) { hi = (int32_t)a; }
        if ((int32_t)b > hi) { hi = (int32_t)b; }

        Wave_DirtyRange(lo, hi);
    }
}

/* 只把一路曲线"旧位置"涉及的行标脏（用于把隐藏的通道擦掉） */
static void Wave_MarkPrev(const int16_t *prev, uint16_t count)
{
    uint16_t i;
    int16_t  a;
    int16_t  b;

    for (i = 0U; i < count; i++)
    {
        a = (i == 0U) ? prev[0] : prev[i - 1U];
        b = prev[i];
        Wave_DirtyRange((a < b) ? (int32_t)a : (int32_t)b,
                        (a > b) ? (int32_t)a : (int32_t)b);
    }
}

/* 把一路曲线叠加到行缓冲上 */
static void Wave_OverlayRow(uint16_t *buf, const int16_t *cur, uint16_t count,
                            int16_t y, uint16_t color)
{
    uint16_t i;
    int16_t  a;
    int16_t  b;

    if (cur[0] == y)
    {
        buf[0] = color;
    }
    b = cur[0];
    for (i = 1U; i < count; i++)
    {
        a = b;
        b = cur[i];
        if ((y >= a) && (y <= b))
        {
            buf[i] = color;
        }
        else if ((y >= b) && (y <= a))
        {
            buf[i] = color;
        }
        else
        {
            /* 本行不覆盖这一列（比如斜线段跳到下一行） */
        }
    }
}

/* 触发电平标记：电平所在行画一条"2 像素亮、2 像素灭"的虚线，
   再在左端画一个小方块，和波形/网格区分开。
   锁定（state=1）用灰色，表示"已找到触发点"；
   未锁定（state=2）用红色，提示"当前没有触发，画面还在自由滚动"。
   虚线只占一半像素，波形压在下面时仍能看清。 */
static void Wave_OverlayTrigger(uint16_t *buf, uint8_t state)
{
    uint16_t x;
    uint16_t color = (state == 1U) ? VIEW_COLOR_LABEL : VIEW_COLOR_SELECT;
    uint16_t last  = (uint16_t)(VIEW_W - 1U);

    for (x = 2U; x < last; x += 4U)
    {
        buf[x] = color;
        if ((uint16_t)(x + 1U) < last)
        {
            buf[x + 1U] = color;
        }
    }

    for (x = 1U; x <= 5U; x++)
    {
        buf[x] = color;
    }
}

/* 行内容指纹（FNV-1a）。用来跳过"标脏了但合成后其实没变"的行——
   矩形波、稳定波形的大部分行都属于这种情况，能再省一大截 SPI 流量。
   32 位哈希碰撞概率极低，万一撞上也只是那一行晚一帧刷新，看不出来。 */
static uint32_t Wave_HashRow(const uint16_t *buf)
{
    uint32_t h = 2166136261UL;
    uint16_t i;

    for (i = 0U; i < VIEW_W; i++)
    {
        h ^= (uint32_t)(buf[i] & 0xFFU);
        h *= 16777619UL;
        h ^= (uint32_t)(buf[i] >> 8);
        h *= 16777619UL;
    }
    return h;
}

/* 把所有脏行合成并推出去。
   trig_state != 0 时，trig_y 所在行还会叠加触发电平标记。 */
static void Wave_FlushRows(uint16_t count, uint8_t show1, uint8_t show2,
                           int16_t trig_y, uint8_t trig_state)
{
    uint16_t row;
    uint16_t x;
    uint16_t y;
    uint16_t color;
    uint32_t hash;

    for (row = 0U; row < VIEW_WAVE_H; row++)
    {
        if (Wave_IsDirty(row) == 0U)
        {
            continue;
        }

        y = (uint16_t)(VIEW_WAVE_Y + row);

        if (Wave_FullRowColor(y, &color) != 0U)
        {
            for (x = 0U; x < VIEW_W; x++)
            {
                s_row_buf[x] = color;
            }
        }
        else
        {
            (void)memcpy(s_row_buf, s_row_normal, sizeof(s_row_buf));
        }

        if (show1 != 0U)
        {
            Wave_OverlayRow(s_row_buf, s_cur_y1, count, (int16_t)y, VIEW_COLOR_CH1);
        }
        if (show2 != 0U)
        {
            Wave_OverlayRow(s_row_buf, s_cur_y2, count, (int16_t)y, VIEW_COLOR_CH2);
        }

        /* 触发电平标记画在波形之上：它是一条"参考线"，不应该被曲线盖住 */
        if ((trig_state != 0U) && ((int16_t)y == trig_y))
        {
            Wave_OverlayTrigger(s_row_buf, trig_state);
        }

        /* 合成结果与屏幕上已有的完全一样就不用再发一遍 */
        hash = Wave_HashRow(s_row_buf);
        if ((s_valid != 0U) && (s_row_hash[row] == hash))
        {
            continue;
        }
        s_row_hash[row] = hash;

        Lcd_SetWindow(0U, y, VIEW_W, 1U);
        Lcd_PushColors(s_row_buf, VIEW_W);
    }
}

void WaveView_DrawTrace(const WaveView_Data *data)
{
    uint16_t count;
    uint16_t i;
    uint8_t  show1;
    uint8_t  show2;
    uint8_t  trig_state;
    int16_t  trig_y = 0;

    if ((data == 0) || (data->count == 0U))
    {
        return;
    }

    count = data->count;
    if (count > VIEW_W)
    {
        count = VIEW_W;
    }
    if (count < 2U)
    {
        return;
    }

    show1 = ((data->show1 != 0U) && (data->ch1 != 0)) ? 1U : 0U;
    show2 = ((data->show2 != 0U) && (data->ch2 != 0)) ? 1U : 0U;

    /* 触发电平线：只有触发开启时才画。电平可能超出 ±5 格的可视范围，
       Wave_SampleToY 会把它钳在波形区的上下边框上。 */
    trig_state = data->trig_state;
    if (trig_state != 0U)
    {
        trig_y = Wave_SampleToY(data->trig_mv, data->vdiv_mv_trig);
    }

    if (s_row_ready == 0U)
    {
        Wave_BuildRowTemplate();
    }

    /* 1. 本帧两条曲线的 y */
    for (i = 0U; i < count; i++)
    {
        s_cur_y1[i] = (show1 != 0U) ? Wave_SampleToY(data->ch1[i], data->vdiv_mv1)
                                    : s_prev_y1[i];
        s_cur_y2[i] = (show2 != 0U) ? Wave_SampleToY(data->ch2[i], data->vdiv_mv2)
                                    : s_prev_y2[i];
    }

    /* 2. 标脏：屏幕状态未知就整片重画 */
    Wave_DirtyClear();

    if (s_valid == 0U)
    {
        Wave_DirtyAll();
    }
    else
    {
        if (show1 != 0U)
        {
            Wave_MarkChannel(s_prev_y1, s_cur_y1, count);
        }
        else
        {
            Wave_MarkPrev(s_prev_y1, count);      /* 隐藏后把旧痕迹擦掉 */
        }
        if (show2 != 0U)
        {
            Wave_MarkChannel(s_prev_y2, s_cur_y2, count);
        }
        else
        {
            Wave_MarkPrev(s_prev_y2, count);
        }

        /* 电平线移动 / 开关 / 锁定状态变化，都要把涉及的行重画：
           旧位置擦掉，新位置画上。 */
        if (s_prev_trig_state != 0U)
        {
            Wave_DirtyRange(s_prev_trig_y, s_prev_trig_y);
        }
        if (trig_state != 0U)
        {
            Wave_DirtyRange(trig_y, trig_y);
        }
    }

    /* 3. 合成脏行并推送 */
    Wave_FlushRows(count, show1, show2, trig_y, trig_state);

    /* 4. 记录本帧位置 */
    (void)memcpy(s_prev_y1, s_cur_y1, (size_t)count * sizeof(s_prev_y1[0]));
    (void)memcpy(s_prev_y2, s_cur_y2, (size_t)count * sizeof(s_prev_y2[0]));
    s_prev_trig_y     = trig_y;
    s_prev_trig_state = trig_state;
    s_valid = 1U;
}

void WaveView_DrawMessage(const char *text)
{
    uint16_t w;
    uint16_t x;
    uint16_t y;

    if (text == 0)
    {
        return;
    }

    w = Lcd_TextWidth(FONT_SMALL, text);
    x = (uint16_t)((VIEW_W - w) / 2U);
    y = (uint16_t)(VIEW_WAVE_MID_Y - (FONT_SMALL_HEIGHT / 2U));

    /* 用背景色铺一条，保证文字在网格上也清楚。
       铺满整行：标定提示会换不同长度的字符串，窄条会留下上一句的残影。 */
    Lcd_FillRect(0U, y, (uint16_t)VIEW_W, (uint16_t)FONT_SMALL_HEIGHT, VIEW_COLOR_BG);
    (void)Lcd_DrawString(x, y, text, FONT_SMALL, VIEW_COLOR_TEXT, VIEW_COLOR_BG);

    s_valid = 0U;                                 /* 文字压在网格上，下次整片重画 */
}

void WaveView_DrawBadge(const char *text, uint16_t fg)
{
    if (text == 0)
    {
        return;
    }
    (void)Lcd_DrawString(4U, (uint16_t)(VIEW_WAVE_Y + 3U), text, FONT_SMALL,
                         fg, VIEW_COLOR_BG);

    s_valid = 0U;
}
