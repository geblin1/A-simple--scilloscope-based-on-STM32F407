/**
  * @file    Lcd.h
  * @brief   ST7789 240x320 IPS 屏驱动（SPI1，横屏 320x240）
  *
  * 分层约定
  * --------
  * 本模块是唯一的"屏"底层：GPIO/SPI 时序、窗口设置、像素推送全部在这里。
  * 上层（View/App）只调用本文件导出的函数，不直接碰 hspi1 与引脚。
  * 本头文件不包含任何 HAL 类型，换屏/换接口时上层代码不需要改。
  *
  * 坐标系（横屏）
  * -------------
  * 原点在左上角，x 向右 0~319，y 向下 0~239。
  * 旋转由内部 LCD_MADCTL 决定，上层始终按横屏坐标使用。
  *
  * 性能提示
  * --------
  * 屏幕颜色是 RGB565（2 B/像素），SPI1 工作在 10.5 Mbit/s，
  * 因此整屏 320x240 = 153600 B 约需 117 ms。
  * 上层应"一次设窗口 + 批量推像素"，不要在循环里逐点设窗口。
  */

#ifndef __LCD_H
#define __LCD_H

#include <stdint.h>

#include "Font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 屏幕尺寸                                                            */
/* ------------------------------------------------------------------ */
#define LCD_WIDTH       320U
#define LCD_HEIGHT      240U

/* ------------------------------------------------------------------ */
/* RGB565 颜色构造                                                    */
/*                                                                    */
/* 5 位红、6 位绿、5 位蓝。参数范围 0~255，超出按上限截断。            */
/* ------------------------------------------------------------------ */
#define LCD_RGB565(r, g, b)   \
    ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | \
                (((uint16_t)(g) & 0xFCU) << 3) | \
                (((uint16_t)(b) & 0xF8U) >> 3)))

/* 常用颜色（RGB565） */
#define LCD_BLACK        0x0000U
#define LCD_WHITE        0xFFFFU
#define LCD_RED          0xF800U
#define LCD_GREEN        0x07E0U
#define LCD_BLUE         0x001FU
#define LCD_YELLOW       0xFFE0U
#define LCD_CYAN         0x07FFU
#define LCD_MAGENTA      0xF81FU
#define LCD_ORANGE       0xFD20U
#define LCD_GRAY         0x8410U

/* ------------------------------------------------------------------ */
/* 设备控制                                                            */
/* ------------------------------------------------------------------ */

/**
  * @brief  初始化屏幕（硬复位 + 寄存器序列 + 开背光 + 清屏为黑色）
  * @note   必须在 MX_SPI1_Init() 与 MX_GPIO_Init() 之后调用
  */
void Lcd_Init(void);

/**
  * @brief  开/关背光
  * @param  on 1 = 点亮，0 = 熄灭
  */
void Lcd_Backlight(uint8_t on);

/**
  * @brief  屏幕宽度（横屏，恒为 320）
  */
uint16_t Lcd_Width(void);

/**
  * @brief  屏幕高度（横屏，恒为 240）
  */
uint16_t Lcd_Height(void);

/* ------------------------------------------------------------------ */
/* 像素操作                                                            */
/* ------------------------------------------------------------------ */

/**
  * @brief  设置写入窗口并定位到其左上角
  * @param  x,y 窗口左上角
  * @param  w,h 窗口宽高（像素，至少 1）
  * @note   之后调用 Lcd_PushColors() 即可把像素按行优先顺序填入
  */
void Lcd_SetWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/**
  * @brief  向当前窗口推送像素
  * @param  colors RGB565 数组
  * @param  count  像素个数
  * @note   超出窗口的部分会由屏幕自行丢弃/回卷，调用方需自行保证数量匹配
  */
void Lcd_PushColors(const uint16_t *colors, uint32_t count);

/**
  * @brief  用单色填充整屏
  */
void Lcd_Fill(uint16_t color);

/**
  * @brief  用单色填充矩形
  */
void Lcd_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
  * @brief  画一个像素
  */
void Lcd_DrawPixel(uint16_t x, uint16_t y, uint16_t color);

/**
  * @brief  画水平线（含起点，长度 w）
  */
void Lcd_DrawHLine(uint16_t x, uint16_t y, uint16_t w, uint16_t color);

/**
  * @brief  画垂直线（含起点，长度 h）
  */
void Lcd_DrawVLine(uint16_t x, uint16_t y, uint16_t h, uint16_t color);

/**
  * @brief  画矩形边框（不含填充）
  */
void Lcd_DrawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/* ------------------------------------------------------------------ */
/* 字符与字符串                                                        */
/* ------------------------------------------------------------------ */

/**
  * @brief  在 (x, y) 画一个字符，背景用 bg 填满字符单元
  * @param  font 字号（FONT_SMALL / FONT_BIG）
  */
void Lcd_DrawChar(uint16_t x, uint16_t y, char ch, Font_Id font,
                  uint16_t fg, uint16_t bg);

/**
  * @brief  画字符串，字符单元内的背景用 bg 填充（不透明）
  * @retval 绘制结束后的 x 坐标（便于连续拼接）
  */
uint16_t Lcd_DrawString(uint16_t x, uint16_t y, const char *text, Font_Id font,
                        uint16_t fg, uint16_t bg);

/**
  * @brief  画字符串，只画点亮的像素，不填背景（用于覆盖在网格/波形上）
  * @retval 绘制结束后的 x 坐标
  */
uint16_t Lcd_DrawStringTransparent(uint16_t x, uint16_t y, const char *text,
                                   Font_Id font, uint16_t fg);

/**
  * @brief  字符串像素宽度（便于右对齐）
  */
uint16_t Lcd_TextWidth(Font_Id font, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* __LCD_H */
