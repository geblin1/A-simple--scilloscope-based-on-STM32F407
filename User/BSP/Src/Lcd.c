/**
  * @file    Lcd.c
  * @brief   ST7789 240x320 IPS 屏驱动（SPI1，横屏 320x240）
  *
  * 硬件连接（见 PROJECT_SPEC.md）
  * -----------------------------
  *   SCK  = PA5   (SPI1_SCK, AF5)
  *   MOSI = PA7   (SPI1_MOSI, AF5)
  *   CS   = PA4   (推挽输出，低有效)
  *   DC   = PB0   (推挽输出，0=命令 1=数据)
  *   RES  = PB1   (推挽输出，低电平复位)
  *   BLK  = PB2   (推挽输出，高电平点亮背光)
  *   MISO 不接：本屏只写不读
  *
  * 写法要点
  * --------
  * 1. 所有像素推送都走 Lcd_SetWindow() + Lcd_PushColors()：一次设窗口，
  *    然后连续送一整块数据。逐点设窗口会把绝大部分时间花在命令开销上。
  * 2. ST7789 要求 RGB565 高字节在前，而 Cortex-M4 是小端，所以必须做
  *    字节交换；这里用 s_tx 分块缓冲完成交换与批量发送，避免占用大块 RAM。
  * 3. 本文件不向上暴露任何 HAL 类型，上层只看到颜色和坐标。
  */

#include "Lcd.h"

#include "main.h"
#include "spi.h"

/* ------------------------------------------------------------------ */
/* 引脚                                                                */
/* ------------------------------------------------------------------ */
#define LCD_CS_PORT     GPIOA
#define LCD_CS_PIN      GPIO_PIN_4
#define LCD_DC_PORT     GPIOB
#define LCD_DC_PIN      GPIO_PIN_0
#define LCD_RES_PORT    GPIOB
#define LCD_RES_PIN     GPIO_PIN_1
#define LCD_BLK_PORT    GPIOB
#define LCD_BLK_PIN     GPIO_PIN_2

#define LCD_CS_LOW()    HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, GPIO_PIN_RESET)
#define LCD_CS_HIGH()   HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, GPIO_PIN_SET)
#define LCD_DC_CMD()    HAL_GPIO_WritePin(LCD_DC_PORT, LCD_DC_PIN, GPIO_PIN_RESET)
#define LCD_DC_DATA()   HAL_GPIO_WritePin(LCD_DC_PORT, LCD_DC_PIN, GPIO_PIN_SET)
#define LCD_RES_LOW()   HAL_GPIO_WritePin(LCD_RES_PORT, LCD_RES_PIN, GPIO_PIN_RESET)
#define LCD_RES_HIGH()  HAL_GPIO_WritePin(LCD_RES_PORT, LCD_RES_PIN, GPIO_PIN_SET)
#define LCD_BLK_ON()    HAL_GPIO_WritePin(LCD_BLK_PORT, LCD_BLK_PIN, GPIO_PIN_SET)
#define LCD_BLK_OFF()   HAL_GPIO_WritePin(LCD_BLK_PORT, LCD_BLK_PIN, GPIO_PIN_RESET)

/* ------------------------------------------------------------------ */
/* 屏幕方向                                                            */
/*                                                                    */
/* 这块屏原生是 240(宽)x320(高)，要横过来显示就需要交换行列。          */
/*   0x00 竖屏 240x320        0x60 横屏 320x240（本工程使用）          */
/*   0xA0 横屏 320x240 翻转   0xC0 竖屏 240x320 翻转                   */
/* 若实机显示上下颠倒或左右镜像，只改这一个宏即可。                    */
/* ------------------------------------------------------------------ */
#define LCD_MADCTL      0x60U

/* SPI 单次传输的像素数：128 像素 = 256 字节，够小不占 RAM，够大省开销 */
#define LCD_TX_CHUNK    128U
#define LCD_SPI_TIMEOUT 100U

/* 字模在字符单元内的垂直居中偏移 */
#define LCD_FONT_OFFSET_SMALL   1U
#define LCD_FONT_OFFSET_BIG     2U

/* ------------------------------------------------------------------ */
/* ST7789 命令                                                        */
/* ------------------------------------------------------------------ */
#define ST7789_SWRESET   0x01U
#define ST7789_SLPOUT    0x11U
#define ST7789_NORON     0x13U
#define ST7789_INVON     0x21U
#define ST7789_DISPON    0x29U
#define ST7789_CASET     0x2AU
#define ST7789_RASET     0x2BU
#define ST7789_RAMWR     0x2CU
#define ST7789_MADCTL    0x36U
#define ST7789_COLMOD    0x3AU
#define ST7789_PORCTRL   0xB2U
#define ST7789_GCTRL     0xB7U
#define ST7789_VCOMS     0xBBU
#define ST7789_LCMCTRL   0xC0U
#define ST7789_VDVVRHEN  0xC2U
#define ST7789_VRHS      0xC3U
#define ST7789_VDVS      0xC4U
#define ST7789_FRCTRL2   0xC6U
#define ST7789_PWCTRL1   0xD0U
#define ST7789_PVGAMCTRL 0xE0U
#define ST7789_NVGAMCTRL 0xE1U

/* 字节交换用的分块缓冲 */
static uint8_t s_tx[LCD_TX_CHUNK * 2U];

/* ================================================================== */
/* 底层读写                                                            */
/* ================================================================== */

static void Lcd_WriteCmd(uint8_t cmd)
{
    LCD_DC_CMD();
    LCD_CS_LOW();
    (void)HAL_SPI_Transmit(&hspi1, &cmd, 1U, LCD_SPI_TIMEOUT);
    LCD_CS_HIGH();
}

static void Lcd_WriteData(const uint8_t *buf, uint16_t len)
{
    if ((buf == 0) || (len == 0U))
    {
        return;
    }
    LCD_DC_DATA();
    LCD_CS_LOW();
    (void)HAL_SPI_Transmit(&hspi1, (uint8_t *)buf, len, LCD_SPI_TIMEOUT);
    LCD_CS_HIGH();
}

/* 发一条命令 + 一段参数；参数为 0 时只发命令 */
static void Lcd_CmdData(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    Lcd_WriteCmd(cmd);
    if ((data != 0) && (len != 0U))
    {
        Lcd_WriteData(data, len);
    }
}

/* 把 count 个同色像素字节填入 s_tx，用于纯色填充 */
static void Lcd_FillTx(uint16_t color, uint32_t pixels)
{
    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)(color & 0xFFU);
    uint32_t i;

    for (i = 0U; i < pixels; i++)
    {
        s_tx[i * 2U]      = hi;
        s_tx[(i * 2U) + 1U] = lo;
    }
}

/* ================================================================== */
/* 设备控制                                                            */
/* ================================================================== */

void Lcd_Backlight(uint8_t on)
{
    if (on != 0U)
    {
        LCD_BLK_ON();
    }
    else
    {
        LCD_BLK_OFF();
    }
}

uint16_t Lcd_Width(void)
{
    return (uint16_t)LCD_WIDTH;
}

uint16_t Lcd_Height(void)
{
    return (uint16_t)LCD_HEIGHT;
}

void Lcd_Init(void)
{
    static const uint8_t porctrl[]  = { 0x0CU, 0x0CU, 0x00U, 0x33U, 0x33U };
    static const uint8_t gctrl[]    = { 0x35U };
    static const uint8_t vcoms[]    = { 0x19U };
    static const uint8_t lcmctrl[]  = { 0x2CU };
    static const uint8_t vdvrvhen[] = { 0x01U };
    static const uint8_t vrhs[]     = { 0x12U };
    static const uint8_t vdvs[]     = { 0x20U };
    static const uint8_t frctrl2[]  = { 0x0FU };
    static const uint8_t pwctrl1[]  = { 0xA4U, 0xA1U };
    static const uint8_t pgamma[]   = { 0xD0U, 0x04U, 0x0DU, 0x11U, 0x13U, 0x2BU,
                                        0x3FU, 0x54U, 0x4CU, 0x18U, 0x0DU, 0x0BU,
                                        0x1FU, 0x23U };
    static const uint8_t ngamma[]   = { 0xD0U, 0x04U, 0x0CU, 0x11U, 0x13U, 0x2CU,
                                        0x3FU, 0x44U, 0x51U, 0x2FU, 0x1FU, 0x1FU,
                                        0x20U, 0x23U };
    uint8_t madctl = (uint8_t)LCD_MADCTL;
    uint8_t colmod = 0x55U;   /* 16 bit/pixel，RGB565 */

    /* 上电默认态：CS 高、DC 高、背光灭 */
    LCD_CS_HIGH();
    LCD_DC_DATA();
    Lcd_Backlight(0U);

    /* 硬复位：RES 低 >= 10us 即可，这里给足 20ms 更稳 */
    LCD_RES_HIGH();
    HAL_Delay(10U);
    LCD_RES_LOW();
    HAL_Delay(20U);
    LCD_RES_HIGH();
    HAL_Delay(120U);

    /* 软复位后必须等待，否则寄存器写入会被忽略 */
    Lcd_WriteCmd(ST7789_SWRESET);
    HAL_Delay(120U);

    Lcd_WriteCmd(ST7789_SLPOUT);
    HAL_Delay(120U);

    Lcd_CmdData(ST7789_COLMOD, &colmod, 1U);
    Lcd_CmdData(ST7789_MADCTL, &madctl, 1U);

    Lcd_CmdData(ST7789_PORCTRL, porctrl, (uint16_t)sizeof(porctrl));
    Lcd_CmdData(ST7789_GCTRL, gctrl, (uint16_t)sizeof(gctrl));
    Lcd_CmdData(ST7789_VCOMS, vcoms, (uint16_t)sizeof(vcoms));
    Lcd_CmdData(ST7789_LCMCTRL, lcmctrl, (uint16_t)sizeof(lcmctrl));
    Lcd_CmdData(ST7789_VDVVRHEN, vdvrvhen, (uint16_t)sizeof(vdvrvhen));
    Lcd_CmdData(ST7789_VRHS, vrhs, (uint16_t)sizeof(vrhs));
    Lcd_CmdData(ST7789_VDVS, vdvs, (uint16_t)sizeof(vdvs));
    Lcd_CmdData(ST7789_FRCTRL2, frctrl2, (uint16_t)sizeof(frctrl2));
    Lcd_CmdData(ST7789_PWCTRL1, pwctrl1, (uint16_t)sizeof(pwctrl1));
    Lcd_CmdData(ST7789_PVGAMCTRL, pgamma, (uint16_t)sizeof(pgamma));
    Lcd_CmdData(ST7789_NVGAMCTRL, ngamma, (uint16_t)sizeof(ngamma));

    /* IPS 屏一般需要反显，如果实机颜色整体反了就去掉这一句 */
    Lcd_WriteCmd(ST7789_INVON);
    HAL_Delay(10U);

    Lcd_WriteCmd(ST7789_NORON);
    HAL_Delay(10U);

    Lcd_Fill(LCD_BLACK);

    Lcd_WriteCmd(ST7789_DISPON);
    HAL_Delay(100U);

    Lcd_Backlight(1U);
}

/* ================================================================== */
/* 窗口与像素                                                          */
/* ================================================================== */

void Lcd_SetWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint8_t caset[4];
    uint8_t raset[4];
    uint16_t x2;
    uint16_t y2;

    if ((w == 0U) || (h == 0U))
    {
        return;
    }
    if ((x >= (uint16_t)LCD_WIDTH) || (y >= (uint16_t)LCD_HEIGHT))
    {
        return;
    }

    x2 = (uint16_t)(x + w - 1U);
    y2 = (uint16_t)(y + h - 1U);
    if (x2 >= (uint16_t)LCD_WIDTH)
    {
        x2 = (uint16_t)(LCD_WIDTH - 1U);
    }
    if (y2 >= (uint16_t)LCD_HEIGHT)
    {
        y2 = (uint16_t)(LCD_HEIGHT - 1U);
    }

    caset[0] = (uint8_t)(x >> 8);
    caset[1] = (uint8_t)(x & 0xFFU);
    caset[2] = (uint8_t)(x2 >> 8);
    caset[3] = (uint8_t)(x2 & 0xFFU);
    Lcd_CmdData(ST7789_CASET, caset, 4U);

    raset[0] = (uint8_t)(y >> 8);
    raset[1] = (uint8_t)(y & 0xFFU);
    raset[2] = (uint8_t)(y2 >> 8);
    raset[3] = (uint8_t)(y2 & 0xFFU);
    Lcd_CmdData(ST7789_RASET, raset, 4U);

    Lcd_WriteCmd(ST7789_RAMWR);
}

void Lcd_PushColors(const uint16_t *colors, uint32_t count)
{
    uint32_t remain = count;
    uint32_t n;
    uint32_t i;

    if ((colors == 0) || (count == 0U))
    {
        return;
    }

    while (remain > 0U)
    {
        n = (remain > LCD_TX_CHUNK) ? (uint32_t)LCD_TX_CHUNK : remain;
        for (i = 0U; i < n; i++)
        {
            uint16_t c = colors[i];

            s_tx[i * 2U]        = (uint8_t)(c >> 8);
            s_tx[(i * 2U) + 1U] = (uint8_t)(c & 0xFFU);
        }
        LCD_DC_DATA();
        LCD_CS_LOW();
        (void)HAL_SPI_Transmit(&hspi1, s_tx, (uint16_t)(n * 2U), LCD_SPI_TIMEOUT);
        LCD_CS_HIGH();

        colors += n;
        remain -= n;
    }
}

void Lcd_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint32_t total;
    uint32_t remain;
    uint32_t n;

    if ((w == 0U) || (h == 0U))
    {
        return;
    }
    if ((x >= (uint16_t)LCD_WIDTH) || (y >= (uint16_t)LCD_HEIGHT))
    {
        return;
    }
    if ((uint16_t)(x + w) > (uint16_t)LCD_WIDTH)
    {
        w = (uint16_t)(LCD_WIDTH - x);
    }
    if ((uint16_t)(y + h) > (uint16_t)LCD_HEIGHT)
    {
        h = (uint16_t)(LCD_HEIGHT - y);
    }

    Lcd_SetWindow(x, y, w, h);

    total  = (uint32_t)w * (uint32_t)h;
    remain = total;
    while (remain > 0U)
    {
        n = (remain > LCD_TX_CHUNK) ? (uint32_t)LCD_TX_CHUNK : remain;
        Lcd_FillTx(color, n);
        LCD_DC_DATA();
        LCD_CS_LOW();
        (void)HAL_SPI_Transmit(&hspi1, s_tx, (uint16_t)(n * 2U), LCD_SPI_TIMEOUT);
        LCD_CS_HIGH();
        remain -= n;
    }
}

void Lcd_Fill(uint16_t color)
{
    Lcd_FillRect(0U, 0U, (uint16_t)LCD_WIDTH, (uint16_t)LCD_HEIGHT, color);
}

void Lcd_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    Lcd_FillRect(x, y, 1U, 1U, color);
}

void Lcd_DrawHLine(uint16_t x, uint16_t y, uint16_t w, uint16_t color)
{
    Lcd_FillRect(x, y, w, 1U, color);
}

void Lcd_DrawVLine(uint16_t x, uint16_t y, uint16_t h, uint16_t color)
{
    Lcd_FillRect(x, y, 1U, h, color);
}

void Lcd_DrawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if ((w == 0U) || (h == 0U))
    {
        return;
    }
    Lcd_DrawHLine(x, y, w, color);
    if (h > 1U)
    {
        Lcd_DrawHLine(x, (uint16_t)(y + h - 1U), w, color);
    }
    if (h > 2U)
    {
        Lcd_DrawVLine(x, (uint16_t)(y + 1U), (uint16_t)(h - 2U), color);
        if (w > 1U)
        {
            Lcd_DrawVLine((uint16_t)(x + w - 1U), (uint16_t)(y + 1U),
                          (uint16_t)(h - 2U), color);
        }
    }
}

/* ================================================================== */
/* 字符                                                                */
/* ================================================================== */

/* 字形的点阵列数（小字体 5，大字体 10） */
static uint16_t Lcd_GlyphCols(Font_Id font)
{
    return (uint16_t)((font == FONT_BIG) ? (FONT_GLYPH_WIDTH * 2U)
                                        : (uint16_t)FONT_GLYPH_WIDTH);
}

/* 字形的点阵行数（小字体 7，大字体 14） */
#define LCD_GLYPH_ROWS(font) \
    ((uint16_t)((font) == FONT_BIG ? (FONT_GLYPH_HEIGHT * 2U) : (FONT_GLYPH_HEIGHT)))

/* 字模在字符单元内的垂直偏移 */
#define LCD_GLYPH_OFFSET(font) \
    ((uint16_t)((font) == FONT_BIG ? LCD_FONT_OFFSET_BIG : LCD_FONT_OFFSET_SMALL))

/**
  * @brief  取字符单元中第 cell_row 行对应的点阵掩码
  * @retval 掩码；返回 LCD_ROW_SKIP 表示这一行完全是间隔，不需要绘制
  */
#define LCD_ROW_SKIP 0xFFFFU

static uint16_t Lcd_GlyphRow(Font_Id font, char ch, uint16_t cell_row)
{
    uint16_t offset = LCD_GLYPH_OFFSET(font);
    uint16_t rows   = LCD_GLYPH_ROWS(font);

    if (cell_row < offset)
    {
        return LCD_ROW_SKIP;
    }
    cell_row = (uint16_t)(cell_row - offset);
    if (cell_row >= rows)
    {
        return LCD_ROW_SKIP;
    }
    return Font_GetRow(font, ch, (uint8_t)cell_row);
}

void Lcd_DrawChar(uint16_t x, uint16_t y, char ch, Font_Id font,
                  uint16_t fg, uint16_t bg)
{
    uint16_t cell_w = (uint16_t)Font_Width(font);
    uint16_t cell_h = (uint16_t)Font_Height(font);
    uint16_t row;
    uint16_t col;
    uint16_t mask;
    uint32_t idx;

    if ((x >= (uint16_t)LCD_WIDTH) || (y >= (uint16_t)LCD_HEIGHT))
    {
        return;
    }
    if (((uint32_t)x + cell_w) > (uint32_t)LCD_WIDTH)
    {
        cell_w = (uint16_t)(LCD_WIDTH - x);
    }
    if (((uint32_t)y + cell_h) > (uint32_t)LCD_HEIGHT)
    {
        cell_h = (uint16_t)(LCD_HEIGHT - y);
    }

    Lcd_SetWindow(x, y, cell_w, cell_h);

    for (row = 0U; row < cell_h; row++)
    {
        uint16_t glyph_cols = Lcd_GlyphCols(font);

        mask = Lcd_GlyphRow(font, ch, row);
        if (mask == LCD_ROW_SKIP)
        {
            mask = 0U;
        }
        for (col = 0U; col < cell_w; col++)
        {
            idx = (uint32_t)(col * 2U);
            if ((col < glyph_cols) && (((mask >> col) & 1U) != 0U))
            {
                s_tx[idx]      = (uint8_t)(fg >> 8);
                s_tx[idx + 1U] = (uint8_t)(fg & 0xFFU);
            }
            else
            {
                s_tx[idx]      = (uint8_t)(bg >> 8);
                s_tx[idx + 1U] = (uint8_t)(bg & 0xFFU);
            }
        }
        LCD_DC_DATA();
        LCD_CS_LOW();
        (void)HAL_SPI_Transmit(&hspi1, s_tx, (uint16_t)(cell_w * 2U), LCD_SPI_TIMEOUT);
        LCD_CS_HIGH();
    }
}

uint16_t Lcd_DrawString(uint16_t x, uint16_t y, const char *text, Font_Id font,
                        uint16_t fg, uint16_t bg)
{
    uint16_t step = (uint16_t)Font_Width(font);

    if (text == 0)
    {
        return x;
    }
    while (*text != '\0')
    {
        Lcd_DrawChar(x, y, *text, font, fg, bg);
        x = (uint16_t)(x + step);
        if (x >= (uint16_t)LCD_WIDTH)
        {
            break;
        }
        text++;
    }
    return x;
}

uint16_t Lcd_DrawStringTransparent(uint16_t x, uint16_t y, const char *text,
                                   Font_Id font, uint16_t fg)
{
    uint16_t step = (uint16_t)Font_Width(font);
    uint16_t cell_h = (uint16_t)Font_Height(font);
    uint16_t glyph_cols = Lcd_GlyphCols(font);
    uint16_t row;
    uint16_t col;
    uint16_t mask;
    uint16_t run_start;
    uint16_t run_len;

    if (text == 0)
    {
        return x;
    }

    while (*text != '\0')
    {
        for (row = 0U; row < cell_h; row++)
        {
            mask = Lcd_GlyphRow(font, *text, row);
            if (mask == LCD_ROW_SKIP)
            {
                continue;   /* 字形外的行整行都不画 */
            }
            col = 0U;
            while (col < glyph_cols)
            {
                /* 跳过熄灭的列 */
                while ((col < glyph_cols) && (((mask >> col) & 1U) == 0U))
                {
                    col++;
                }
                run_start = col;
                while ((col < glyph_cols) && (((mask >> col) & 1U) != 0U))
                {
                    col++;
                }
                run_len = (uint16_t)(col - run_start);
                if (run_len > 0U)
                {
                    Lcd_FillRect((uint16_t)(x + run_start), (uint16_t)(y + row),
                                 run_len, 1U, fg);
                }
            }
        }
        x = (uint16_t)(x + step);
        if (x >= (uint16_t)LCD_WIDTH)
        {
            break;
        }
        text++;
    }
    return x;
}

uint16_t Lcd_TextWidth(Font_Id font, const char *text)
{
    return Font_TextWidth(font, text);
}
