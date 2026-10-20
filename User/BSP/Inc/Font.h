/**
  * @file    Font.h
  * @brief   ASCII 点阵字模访问接口
  *
  * 设计说明
  * --------
  * LCD 驱动只关心"一个字符占多宽多高、某一行点亮哪几列"，不关心字模
  * 怎么存。所以本模块对外只暴露四个函数，内部结构（5x7 点阵、如何放大）
  * 完全不外露。将来换字体只需改 Font.c，Lcd.c 与上层代码都不动。
  *
  * 两套字号
  * --------
  *   FONT_SMALL  6x9   字符区 5x7 + 右侧 1 px、下方 2 px 间隔
  *   FONT_BIG    12x18  由同一张表整数 2 倍放大，不额外占 Flash
  *
  * 字符集约定
  * ----------
  * 支持 ASCII 0x20~0x7E。小写字母按大写渲染，
  * 其余不可显示字符一律显示为 '?'。
  */

#ifndef __FONT_H
#define __FONT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 字号                                                                */
/* ------------------------------------------------------------------ */
typedef enum
{
    FONT_SMALL = 0,   /* 6x9 px：状态栏 / 参数栏 / 菜单列表 */
    FONT_BIG   = 1    /* 12x18 px：大号读数 / 标题（2 倍整数放大） */
} Font_Id;

/* ------------------------------------------------------------------ */
/* 尺寸（含间隔）                                                      */
/* ------------------------------------------------------------------ */
#define FONT_SMALL_WIDTH    6U
#define FONT_SMALL_HEIGHT   9U
#define FONT_BIG_WIDTH      (FONT_SMALL_WIDTH  * 2U)
#define FONT_BIG_HEIGHT     (FONT_SMALL_HEIGHT * 2U)

/* 单元内真正有点阵的区域（不含间隔） */
#define FONT_GLYPH_WIDTH    5U
#define FONT_GLYPH_HEIGHT   7U

/* ------------------------------------------------------------------ */
/* 字符集范围                                                          */
/* ------------------------------------------------------------------ */
#define FONT_ASCII_FIRST    0x20U
#define FONT_ASCII_LAST     0x7EU

/* ------------------------------------------------------------------ */
/* 接口                                                                */
/* ------------------------------------------------------------------ */

/**
  * @brief  指定字号的字符宽度（含间隔，像素）
  * @param  id 字号
  * @retval 像素宽度
  */
uint8_t Font_Width(Font_Id id);

/**
  * @brief  指定字号的字符高度（含间隔，像素）
  * @param  id 字号
  * @retval 像素高度
  */
uint8_t Font_Height(Font_Id id);

/**
  * @brief  取一个字符某一行的点阵掩码
  * @param  id  字号
  * @param  ch  字符（ASCII 0x20~0x7E）
  * @param  row 行号，0 = 最上面一行；越界返回 0
  * @retval 位掩码，bit n 对应从左往右第 n 列，1 = 该像素点亮
  */
uint16_t Font_GetRow(Font_Id id, char ch, uint8_t row);

/**
  * @brief  字符串在指定字号下的像素宽度（不含结尾 '\0'）
  * @param  id    字号
  * @param  text  以 '\0' 结尾的字符串
  * @retval 像素宽度
  */
uint16_t Font_TextWidth(Font_Id id, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* __FONT_H */
