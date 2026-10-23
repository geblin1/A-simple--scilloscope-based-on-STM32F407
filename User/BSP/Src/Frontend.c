/**
  * @file    Frontend.c
  * @brief   模拟前端标定实现（每个通道、每个量程各一套）
  *
  * 标定模型（详见 FRONTEND_BIAS_DESIGN.md 第七节）
  * ------------------------------------------------
  * ADC 引脚是一个三支路节点：1 kΩ 到 3.3 V、1 kΩ 到 GND、Rs 到信号源。
  * 解基尔霍夫电流定律得
  *
  *     V_pin = 1.65 V + (V_in − 1.65 V) × g ,   g = 500 / (500 + Rs)
  *
  * 于是   零点（V_in = 0）= 1.65 V × (1 − g)
  *        输入 mV = (V_pin − 零点) / g
  *
  * 设备上的两点标定不去算 g，而是直接测两个码值：
  *     zero_raw = 接 GND 时的平均码值
  *     span_raw = 接 3.3 V 时的平均码值
  * 用 (span_raw − zero_raw) 对应 3300 mV 反推比例，天然吸收电阻公差、
  * 偏置偏差和 ADC 误差：
  *
  *     输入 mV = (raw − zero_raw) × 3300 / (span_raw − zero_raw)
  *
  * 等价于 gain_num = 4095、gain_den = span_raw − zero_raw。
  *
  * flash 里的存法
  * --------------
  * 只存每个通道/量程的 zero_raw 与 span_raw（8 个 uint16），派生参数
  * 上电时重算。存到 1 MB 器件的最后一个扇区（扇区 11，0x080E0000）：
  * 代码只有 ~55 KB，重新烧写程序不会碰到这个扇区。
  */

#include "Frontend.h"

#include <string.h>

#include "main.h"       /* HAL：标定结果写 flash */

/* 每个通道、每个量程一套：索引 [通道-1][量程] */
#define FRONTEND_CAL_CH_COUNT     2U
#define FRONTEND_CAL_RANGE_COUNT  2U

/* 标定结果在 flash 中的位置（1 MB 器件的扇区 11） */
#define FRONTEND_CAL_FLASH_ADDR   0x080E0000UL
#define FRONTEND_CAL_FLASH_SECTOR FLASH_SECTOR_11
#define FRONTEND_CAL_MAGIC        0x314C4143UL    /* "CAL1" */
#define FRONTEND_CAL_VERSION      1U

/* 两点码值差的最小值：再小说明两次接的是同一个电位（没接信号） */
#define FRONTEND_SPAN_MIN_RAW     32

/* 两个量程下 3.3 V − GND 的码值差预期区间（设计值 ±35%）。
   用来识别"跳线位置和 V/div 档位不一致"：错配时码值差会落在另一个量程的
   数量级上，此时拒绝标定，避免把错配"固化"成标定值。 */
#define FRONTEND_SPAN_WIDE_MIN    125
#define FRONTEND_SPAN_WIDE_MAX    270
#define FRONTEND_SPAN_SMALL_MIN   950
#define FRONTEND_SPAN_SMALL_MAX   1900

/* 一种量程、一个通道的标定参数（运行时派生，不直接存 flash） */
typedef struct
{
    int32_t zero_raw;      /* 输入 0 V 时的码值（实测） */
    int32_t zero_mv;       /* 输入 0 V 时的引脚电压 */
    int32_t gain_num;      /* 输入 mV = (码值 − 零点) × gain_num / gain_den */
    int32_t gain_den;
    int32_t min_mv;        /* 可测下限（负） */
    int32_t max_mv;        /* 可测上限（正） */
    int32_t lsb_milli_mv;  /* 每码值对应 0.001 mV */
} Frontend_Cal;

/* flash 里保存的内容：魔数 + 8 个实测码值 + 校验和（28 字节 = 7 个字） */
typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint16_t zero_raw[FRONTEND_CAL_CH_COUNT][FRONTEND_CAL_RANGE_COUNT];
    uint16_t span_raw[FRONTEND_CAL_CH_COUNT][FRONTEND_CAL_RANGE_COUNT];
    uint32_t checksum;
} Frontend_CalFlash;

/* 设计默认值：没有有效标定时的兜底（下标 = 量程：WIDE=0 / SMALL=1）。
   两个都是绝对码值：零点取设计模型值，span 取"接 3.3 V 时的码值"
   （= 零点 + 模型码值差：WIDE 195、SMALL 1453）；两个通道相同。 */
static const uint16_t k_default_zero_raw[FRONTEND_CAL_RANGE_COUNT] = { 1950U, 1321U };
static const uint16_t k_default_span_raw[FRONTEND_CAL_RANGE_COUNT] = { 2145U, 2774U };

static Frontend_Cal s_cal[FRONTEND_CAL_CH_COUNT][FRONTEND_CAL_RANGE_COUNT];
static uint16_t     s_zero_raw[FRONTEND_CAL_CH_COUNT][FRONTEND_CAL_RANGE_COUNT];
static uint16_t     s_span_raw[FRONTEND_CAL_CH_COUNT][FRONTEND_CAL_RANGE_COUNT];

/* 上电默认小量程：与默认 V/div（500 mV/div）对应。
   两路前端可以各自处在不同量程（跳线乙分别设置），所以按通道各存一份。 */
static Frontend_Range s_range[FRONTEND_CAL_CH_COUNT] =
{
    FRONTEND_RANGE_SMALL, FRONTEND_RANGE_SMALL
};

/* ------------------------------------------------------------------ */
/* 内部工具                                                            */
/* ------------------------------------------------------------------ */

/* 通道号 -> 数组下标：1/2 之外的值按 CH1 处理 */
static uint8_t Frontend_Ch_Index(uint8_t channel)
{
    return (channel == FRONTEND_CH2) ? 1U : 0U;
}

/* 某通道当前量程对应的数组下标（Frontend_Range 的枚举值就是下标） */
static uint8_t Frontend_Range_Index(uint8_t channel)
{
    return (uint8_t)s_range[Frontend_Ch_Index(channel)];
}

static Frontend_Cal *Frontend_Cal_Of(uint8_t channel)
{
    return &s_cal[Frontend_Ch_Index(channel)][Frontend_Range_Index(channel)];
}

/* 校验和：结构体除最后一个 checksum 字之外所有 32 位字之和。
   用 memcpy 取值，避免把 flash 指针强转成 uint32_t* 的别名问题。 */
static uint32_t Frontend_Cal_Checksum(const Frontend_CalFlash *flash)
{
    uint32_t words[(sizeof(Frontend_CalFlash) / 4U)];
    uint32_t sum = 0U;
    uint32_t i;

    (void)memcpy(words, flash, sizeof(words));
    for (i = 0U; i < ((uint32_t)(sizeof(Frontend_CalFlash) / 4U) - 1U); i++)
    {
        sum += words[i];
    }
    return sum;
}

/* 两个码值是否构成一组合理的两点（零点 < 满点，且拉开足够距离） */
static uint8_t Frontend_Cal_PairValid(uint16_t zero_raw, uint16_t span_raw)
{
    uint32_t diff;

    if ((zero_raw > (uint16_t)FRONTEND_RAW_FULL) ||
        (span_raw > (uint16_t)FRONTEND_RAW_FULL) ||
        (span_raw <= zero_raw))
    {
        return 0U;
    }

    diff = (uint32_t)span_raw - (uint32_t)zero_raw;
    return (diff >= FRONTEND_SPAN_MIN_RAW) ? 1U : 0U;
}

/* 某个量程下"3.3 V 与 GND 的码值差"的合理区间。
   用来同时识别"跳线/档位错配"和"旧格式 flash 数据"。 */
static uint8_t Frontend_Cal_DiffInWindow(uint8_t range_idx, int32_t diff)
{
    if (range_idx == (uint8_t)FRONTEND_RANGE_WIDE)
    {
        return ((diff >= FRONTEND_SPAN_WIDE_MIN) && (diff <= FRONTEND_SPAN_WIDE_MAX)) ? 1U : 0U;
    }
    return ((diff >= FRONTEND_SPAN_SMALL_MIN) && (diff <= FRONTEND_SPAN_SMALL_MAX)) ? 1U : 0U;
}

/* flash 条目按"绝对码值"解释是否合理：zero = 接 GND 的码值，span = 接 3.3 V 的码值 */
static uint8_t Frontend_Cal_EntryValid(uint8_t range_idx, uint16_t zero_raw, uint16_t span_raw)
{
    if (Frontend_Cal_PairValid(zero_raw, span_raw) == 0U)
    {
        return 0U;
    }
    return Frontend_Cal_DiffInWindow(range_idx, (int32_t)span_raw - (int32_t)zero_raw);
}

/* 兼容 2026-10-23 之前的旧格式：未标定的条目把 span 存成了"码值差"（195 / 1453），
   而不是绝对码值。旧值本身落在该量程的码值差窗口内、且 zero + 差值不越界时，
   可以安全迁移成绝对码值。 */
static uint8_t Frontend_Cal_EntryLegacy(uint8_t range_idx, uint16_t zero_raw, uint16_t span_raw)
{
    if (Frontend_Cal_DiffInWindow(range_idx, (int32_t)span_raw) == 0U)
    {
        return 0U;
    }
    return (((uint32_t)zero_raw + (uint32_t)span_raw) <= (uint32_t)FRONTEND_RAW_FULL) ? 1U : 0U;
}

/* 由实测两点派生运行时参数。
   注意：s_zero_raw 是"输入 0 V 时的码值"，s_span_raw 是"输入 3.3 V 时的码值"，
   两者都是绝对码值；换算只用它们的差 (span − zero)，不能用 span 本身。 */
static void Frontend_Cal_Apply(uint8_t ch_idx, uint8_t range_idx)
{
    Frontend_Cal *cal  = &s_cal[ch_idx][range_idx];
    int32_t       zero = (int32_t)s_zero_raw[ch_idx][range_idx];
    int32_t       span = (int32_t)s_span_raw[ch_idx][range_idx];
    int32_t       span_diff = span - zero;

    if (span_diff < FRONTEND_SPAN_MIN_RAW)
    {
        span_diff = FRONTEND_SPAN_MIN_RAW;  /* 防御：绝不留下 0 分母 */
    }

    cal->zero_raw = zero;
    cal->gain_num = (int32_t)FRONTEND_RAW_FULL;
    cal->gain_den = span_diff;

    cal->zero_mv = (int32_t)((((int64_t)zero * FRONTEND_VREF_MV) +
                              (FRONTEND_RAW_FULL / 2)) / FRONTEND_RAW_FULL);
    cal->min_mv  = (int32_t)(((int64_t)(-zero) * FRONTEND_VREF_MV) / span_diff);
    cal->max_mv  = (int32_t)(((int64_t)(FRONTEND_RAW_FULL - zero) * FRONTEND_VREF_MV) / span_diff);
    cal->lsb_milli_mv = (int32_t)(((int64_t)FRONTEND_VREF_MV * 1000) / span_diff);
}

/* ------------------------------------------------------------------ */
/* 初始化：从 flash 读回标定                                            */
/* ------------------------------------------------------------------ */

void Frontend_Init(void)
{
    const Frontend_CalFlash *flash = (const Frontend_CalFlash *)FRONTEND_CAL_FLASH_ADDR;
    uint8_t  valid = 0U;
    uint8_t  ch;
    uint8_t  r;
    uint16_t zero;
    uint16_t span;

    /* 先看魔数、版本和校验和；再逐个校验四点数据是否在合理范围内。
       任何一项不对就整体退回设计默认值，避免"半套标定"。 */
    if ((flash->magic == FRONTEND_CAL_MAGIC) &&
        (flash->version == FRONTEND_CAL_VERSION) &&
        (Frontend_Cal_Checksum(flash) == flash->checksum))
    {
        valid = 1U;

        for (ch = 0U; ch < FRONTEND_CAL_CH_COUNT; ch++)
        {
            for (r = 0U; r < FRONTEND_CAL_RANGE_COUNT; r++)
            {
                if ((Frontend_Cal_EntryValid(r, flash->zero_raw[ch][r],
                                             flash->span_raw[ch][r]) == 0U) &&
                    (Frontend_Cal_EntryLegacy(r, flash->zero_raw[ch][r],
                                              flash->span_raw[ch][r]) == 0U))
                {
                    valid = 0U;
                }
            }
        }
    }

    for (ch = 0U; ch < FRONTEND_CAL_CH_COUNT; ch++)
    {
        for (r = 0U; r < FRONTEND_CAL_RANGE_COUNT; r++)
        {
            if (valid != 0U)
            {
                zero = flash->zero_raw[ch][r];
                span = flash->span_raw[ch][r];

                if (Frontend_Cal_EntryValid(r, zero, span) == 0U)
                {
                    /* 旧格式：span 存的是码值差，迁移成"接 3.3 V 的绝对码值" */
                    span = (uint16_t)((uint32_t)zero + (uint32_t)span);
                }

                s_zero_raw[ch][r] = zero;
                s_span_raw[ch][r] = span;
            }
            else
            {
                s_zero_raw[ch][r] = k_default_zero_raw[r];
                s_span_raw[ch][r] = k_default_span_raw[r];
            }

            Frontend_Cal_Apply(ch, r);
        }
    }

    s_range[0] = FRONTEND_RANGE_SMALL;
    s_range[1] = FRONTEND_RANGE_SMALL;
}

/* ------------------------------------------------------------------ */
/* 量程选择                                                            */
/* ------------------------------------------------------------------ */

void Frontend_SetRange(uint8_t channel, Frontend_Range range)
{
    s_range[Frontend_Ch_Index(channel)] =
        (range == FRONTEND_RANGE_WIDE) ? FRONTEND_RANGE_WIDE : FRONTEND_RANGE_SMALL;
}

Frontend_Range Frontend_GetRange(uint8_t channel)
{
    return s_range[Frontend_Ch_Index(channel)];
}

const char *Frontend_RangeLetter(uint8_t channel)
{
    return (s_range[Frontend_Ch_Index(channel)] == FRONTEND_RANGE_WIDE) ? "W" : "S";
}

const char *Frontend_RangeName(uint8_t channel)
{
    return (s_range[Frontend_Ch_Index(channel)] == FRONTEND_RANGE_WIDE) ? "WIDE" : "SMALL";
}

const char *Frontend_JumperHint(uint8_t channel)
{
    return (s_range[Frontend_Ch_Index(channel)] == FRONTEND_RANGE_WIDE) ? "OPEN" : "MID";
}

/* ------------------------------------------------------------------ */
/* 换算                                                                */
/* ------------------------------------------------------------------ */

int32_t Frontend_RawToMv(uint8_t channel, uint16_t raw)
{
    const Frontend_Cal *cal = Frontend_Cal_Of(channel);
    int64_t codes;

    if (cal->gain_den <= 0)
    {
        return 0;       /* Frontend_Init() 还没调用：避免除零 */
    }

    codes = (int64_t)((int32_t)raw - cal->zero_raw);

    /* 输入 mV = (码值 − 零点) × VREF/FULL × 增益倒数
       中间用 int64：小量程时 4095 × 3300 × 4095 ≈ 5.5e10，超过 int32。 */
    return (int32_t)(codes * (int64_t)FRONTEND_VREF_MV * (int64_t)cal->gain_num
                     / ((int64_t)FRONTEND_RAW_FULL * (int64_t)cal->gain_den));
}

/* ------------------------------------------------------------------ */
/* 两点标定                                                            */
/* ------------------------------------------------------------------ */

uint8_t Frontend_Calibrate(uint8_t channel, uint16_t zero_raw, uint16_t span_raw)
{
    uint8_t  ch   = Frontend_Ch_Index(channel);
    uint8_t  r    = Frontend_Range_Index(channel);
    uint32_t diff;

    if (Frontend_Cal_PairValid(zero_raw, span_raw) == 0U)
    {
        return 0U;
    }

    diff = (uint32_t)span_raw - (uint32_t)zero_raw;

    /* 码值差应该落在当前量程的设计区间内。跳线乙位置和 V/div 不一致时，
       差值是另一个量程的数量级，这里直接拒绝，避免把"用错量程"的数据
       写成标定值（那会把错配固化成正常状态）。 */
    if (s_range[ch] == FRONTEND_RANGE_WIDE)
    {
        if ((diff < FRONTEND_SPAN_WIDE_MIN) || (diff > FRONTEND_SPAN_WIDE_MAX))
        {
            return 0U;
        }
    }
    else
    {
        if ((diff < FRONTEND_SPAN_SMALL_MIN) || (diff > FRONTEND_SPAN_SMALL_MAX))
        {
            return 0U;
        }
    }

    s_zero_raw[ch][r] = zero_raw;
    s_span_raw[ch][r] = span_raw;
    Frontend_Cal_Apply(ch, r);

    return 1U;
}

uint8_t Frontend_CalSave(void)
{
    static Frontend_CalFlash store;     /* 28 字节，放静态区，别占任务栈 */
    uint32_t words[(sizeof(Frontend_CalFlash) / 4U)];
    FLASH_EraseInitTypeDef erase;
    uint32_t sector_error = 0U;
    uint32_t i;
    uint8_t  ch;
    uint8_t  r;
    uint8_t  ok = 1U;

    store.magic   = FRONTEND_CAL_MAGIC;
    store.version = FRONTEND_CAL_VERSION;

    for (ch = 0U; ch < FRONTEND_CAL_CH_COUNT; ch++)
    {
        for (r = 0U; r < FRONTEND_CAL_RANGE_COUNT; r++)
        {
            store.zero_raw[ch][r] = s_zero_raw[ch][r];
            store.span_raw[ch][r] = s_span_raw[ch][r];
        }
    }

    store.checksum = Frontend_Cal_Checksum(&store);
    (void)memcpy(words, &store, sizeof(words));

    (void)HAL_FLASH_Unlock();

    /* 上一次操作可能留下错误标志，先清掉，否则擦除会直接返回失败 */
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    (void)memset(&erase, 0, sizeof(erase));
    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Banks        = FLASH_BANK_1;
    erase.Sector       = FRONTEND_CAL_FLASH_SECTOR;
    erase.NbSectors    = 1U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
    {
        ok = 0U;
    }
    else
    {
        for (i = 0U; i < ((uint32_t)(sizeof(Frontend_CalFlash) / 4U)); i++)
        {
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                  FRONTEND_CAL_FLASH_ADDR + (i * 4U),
                                  words[i]) != HAL_OK)
            {
                ok = 0U;
                break;
            }
        }
    }

    (void)HAL_FLASH_Lock();

    return ok;
}

/* ------------------------------------------------------------------ */
/* 查询接口                                                            */
/* ------------------------------------------------------------------ */

int32_t Frontend_ZeroRaw(uint8_t channel)
{
    return Frontend_Cal_Of(channel)->zero_raw;
}

int32_t Frontend_ZeroMv(uint8_t channel)
{
    return Frontend_Cal_Of(channel)->zero_mv;
}

int32_t Frontend_MinMv(uint8_t channel)
{
    return Frontend_Cal_Of(channel)->min_mv;
}

int32_t Frontend_MaxMv(uint8_t channel)
{
    return Frontend_Cal_Of(channel)->max_mv;
}

int32_t Frontend_MvPerLsbMilli(uint8_t channel)
{
    return Frontend_Cal_Of(channel)->lsb_milli_mv;
}
