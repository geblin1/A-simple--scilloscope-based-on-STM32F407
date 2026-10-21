/**
  * @file    Trigger.c
  * @brief   边沿触发搜索实现（见 Trigger.h 的文件头说明）
  */

#include "Trigger.h"

/* 把"穿越点落在 [v0, v1] 之间的比例位置"换算成 Q16，并夹到 [0, 65535]。
   v0/v1 与 level 的大小关系由调用处保证（上升沿 v0 < level < v1，
   下降沿 v0 > level > v1），所以这里只需要处理除零和端点误差。 */
static uint16_t Trigger_FracQ16(int32_t v0, int32_t v1, int32_t level)
{
    int64_t num = (int64_t)level - (int64_t)v0;
    int64_t den = (int64_t)v1 - (int64_t)v0;
    int64_t frac;

    if (den == 0)
    {
        return 0U;
    }

    frac = (num * 65536) / den;
    if (frac < 0)
    {
        frac = 0;
    }
    else if (frac > 65535)
    {
        frac = 65535;
    }

    return (uint16_t)frac;
}

Trigger_Point Trigger_Find(const int32_t *mv, uint16_t count, const Trigger_Config *cfg)
{
    Trigger_Point out;
    uint32_t      k;
    uint32_t      first;
    uint32_t      last;
    int32_t       lo_th;
    int32_t       hi_th;
    uint8_t       rising;

    out.index    = -1;
    out.frac_q16 = 0U;

    if ((mv == 0) || (cfg == 0))
    {
        return out;
    }

    if ((cfg->edge != TRIGGER_EDGE_RISING) &&
        (cfg->edge != TRIGGER_EDGE_FALLING))
    {
        return out;
    }

    /* 至少要两个样本才能判"前一拍 -> 当前拍"的穿越 */
    if (count < 2U)
    {
        return out;
    }

    /* 窗口必须放得下，否则 index+1 和触发点右侧的显示窗都会越界 */
    if (((uint32_t)cfg->pre_samples + (uint32_t)cfg->post_samples) >=
        (uint32_t)count)
    {
        return out;
    }

    lo_th = cfg->level_mv - cfg->hysteresis_mv;
    hi_th = cfg->level_mv + cfg->hysteresis_mv;

    /* index = 0 没有"前一拍"可比，最早从 1 开始；
       last = count-1-post 同时保证 index+1 < count、右侧留够 post 个点。 */
    first = (cfg->pre_samples >= 1U) ? (uint32_t)cfg->pre_samples : 1U;
    last  = (uint32_t)count - 1U - (uint32_t)cfg->post_samples;

    if (first > last)
    {
        return out;
    }

    rising = (cfg->edge == TRIGGER_EDGE_RISING) ? 1U : 0U;

    for (k = first; k <= last; k++)
    {
        if (rising != 0U)
        {
            if ((mv[k] <= lo_th) && (mv[k + 1U] >= hi_th))
            {
                out.index    = (int32_t)k;
                out.frac_q16 = Trigger_FracQ16(mv[k], mv[k + 1U], cfg->level_mv);
                return out;
            }
        }
        else
        {
            if ((mv[k] >= hi_th) && (mv[k + 1U] <= lo_th))
            {
                out.index    = (int32_t)k;
                out.frac_q16 = Trigger_FracQ16(mv[k], mv[k + 1U], cfg->level_mv);
                return out;
            }
        }
    }

    return out;
}
