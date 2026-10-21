/**
  * @file    Trigger.h
  * @brief   边沿触发搜索（纯整数运算，可脱离硬件单独测试）
  *
  * 它解决什么问题
  * --------------
  * 不触发时，每帧采集窗口的起点是随机的，周期信号在屏幕上就会左右游走
  * （看起来"抖"）。触发的作用是：在整段原始样本里找到"波形按指定方向
  * 穿过设定电平"的那一点，把这一点固定到屏幕的同一列（本工程放在中线），
  * 于是相邻两帧的相位一致，波形就"钉"在屏幕上不动了。
  *
   * 为什么要返回亚采样位置（frac_q16）
  * ----------------------------------
  * 采样时钟和被测信号是异步的，"波形穿过电平"的真实时刻绝大多数落在两个
  * 采样点之间。只把触发点对齐到采样点 k，显示窗口的相位就会随采样网格相对
  * 信号的亚采样偏移而每帧抖动 ±1 个采样点（屏幕上 ±1 像素）；而一像素的横向
  * 抖动会让整条曲线的每个像素都变，增量刷新的行指纹全部失效，刷新率就会
  * 掉回"整屏重画"的水平。返回 k-1 与 k 之间的比例位置，上层据此在原始
  * 样本上做线性插值，把显示窗对齐到真实穿越时刻，帧与帧之间就只剩噪声差异，
  * 增量刷新才真正生效，触发也才真正"钉"得死。
  *
  * 迟滞（hysteresis）
  * -----------------
  * 判沿用两个阈值：高阈值 = level + hyst，低阈值 = level - hyst。
  *   上升沿：前一拍 <= 低阈值，且当前拍 >= 高阈值
  *   下降沿：前一拍 >= 高阈值，且当前拍 <= 低阈值
  * 没有迟滞时，叠加在触发电平附近的噪声会让同一段波形反复穿越，
  * 触发点随机跳动，画面看起来还是在抖。迟滞把这段噪声"吃掉"。
  * hyst 传 0 时退化为普通的单阈值比较。
  *
  * 层依赖
  * ------
  * 只依赖 <stdint.h>，不包含 HAL / FreeRTOS，也不做任何硬件访问。
  */

#ifndef __TRIGGER_H
#define __TRIGGER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 触发边沿 */
typedef enum
{
    TRIGGER_EDGE_OFF     = 0,   /**< 关闭触发：不搜索，显示窗口回到整段的中间 */
    TRIGGER_EDGE_RISING  = 1,   /**< 上升沿触发 */
    TRIGGER_EDGE_FALLING = 2    /**< 下降沿触发 */
} Trigger_Edge;

/* 一次搜索的配置。
   搜索的下标范围是 [pre_samples, count - 1 - post_samples]：
   命中的 index 表示"穿越发生在这拍和下一拍之间"，显示窗口起点 =
   index - pre_samples（本工程 pre_samples = 160，正好是"触发点左侧
   160 点 + 右侧 160 点"的半屏结构）。左边要留够 pre_samples 个点，
   最右侧还要留 post_samples 个点给"触发之后"的显示窗口。 */
typedef struct
{
    int32_t     level_mv;      /**< 触发电平（mV，带符号） */
    int32_t     hysteresis_mv; /**< 迟滞半宽（mV），>= 0 */
    Trigger_Edge edge;         /**< 触发边沿 */
    uint16_t    pre_samples;   /**< 触发点左侧保留的样本数 */
    uint16_t    post_samples;  /**< 触发点右侧保留的样本数 */
} Trigger_Config;

/* 一次命中的触发点。
   穿越时刻落在 index 与 index+1 之间：
     frac_q16 == 0     表示刚好在 index 这一拍穿过去；
     frac_q16 == 65535 表示接近 index+1 才穿过去。
   上层用线性插值就能把显示窗口对齐到真实穿越时刻（见文件头说明）。 */
typedef struct
{
    int32_t  index;     /**< 穿越点前一刻的采样下标；< 0 表示没找到 */
    uint16_t frac_q16;  /**< 穿越时刻在 index..index+1 之间的位置（Q16） */
} Trigger_Point;

/**
  * @brief  在一整段已换算成 mV 的样本里找第一个有效的触发点
  * @param  mv    样本数组（mV，带符号）
  * @param  count 样本数，至少 2
  * @param  cfg   触发配置，不能为 0
  * @retval index >= 0  命中；保证 index >= 1、index+1 < count
  * @retval index <  0  没找到（边沿关闭 / 参数非法 / 没有任何一次有效穿越）
  */
Trigger_Point Trigger_Find(const int32_t *mv, uint16_t count, const Trigger_Config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* __TRIGGER_H */
