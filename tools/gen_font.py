#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_font.py —— 生成 LCD 驱动用的 ASCII 点阵字模（User/BSP/Src/Font.c 与 Font.h）

为什么手写点阵而不用 TrueType
------------------------------
Pillow 的 Consolas / load_default() 在 10~13 px 下都是抗锯齿渲染：
细笔画（'5' 的左竖、中间的横）会掉到阈值以下，字形残缺；把阈值调低又
会把笔画糊成 2 px 的粗黑块。对 1-bit TFT 来说，只有"设计时就是 1 px 笔画"
的点阵字体才干净。所以这里直接把 5x7 点阵写在源码里——改起来看得见。

用法
----
    python tools/gen_font.py                # 生成 Font.c / Font.h
    python tools/gen_font.py --preview 0A5  # 只在终端打印这几个字符，不写文件

输出
----
    s_font_5x7[95][7]，每行一个 uint8_t，bit0 = 最左边一列。
    FONT_BIG 由同一张表 2 倍整数放大，不额外占 Flash。
"""

import argparse
import os
import sys

ASCII_FIRST = 0x20
ASCII_LAST = 0x7E

GLYPH_W = 5
GLYPH_H = 7
CELL_W = 6            # 字形 5 px + 右侧 1 px 间隔
CELL_H = 9            # 字形 7 px + 下方 2 px 间隔
BIG_SCALE = 2

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(ROOT, "User", "BSP", "Src", "Font.c")
OUT_H = os.path.join(ROOT, "User", "BSP", "Inc", "Font.h")

# --------------------------------------------------------------------------
# 5x7 点阵：'#' = 点亮，'.' = 熄灭；每字符 7 行、每行 5 列
# --------------------------------------------------------------------------
GLYPHS = {
    " ": (".....", ".....", ".....", ".....", ".....", ".....", "....."),
    "!": ("..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."),
    '"': (".#.#.", ".#.#.", ".....", ".....", ".....", ".....", "....."),
    "#": (".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#."),
    "$": ("..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#.."),
    "%": ("##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##"),
    "&": (".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#"),
    "'": ("..#..", "..#..", ".....", ".....", ".....", ".....", "....."),
    "(": ("...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."),
    ")": (".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."),
    "*": (".....", "..#..", "#.#.#", ".###.", "#.#.#", "..#..", "....."),
    "+": (".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."),
    ",": (".....", ".....", ".....", ".....", "..##.", "..#..", ".#..."),
    "-": (".....", ".....", ".....", "#####", ".....", ".....", "....."),
    ".": (".....", ".....", ".....", ".....", ".....", "..##.", "..##."),
    "/": ("....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."),
    "0": (".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."),
    "1": ("..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."),
    "2": (".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"),
    "3": ("####.", "....#", "....#", ".###.", "....#", "....#", "####."),
    "4": ("...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."),
    "5": ("#####", "#....", "####.", "....#", "....#", "#...#", ".###."),
    "6": ("..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."),
    "7": ("#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."),
    "8": (".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."),
    "9": (".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."),
    ":": (".....", "..##.", "..##.", ".....", "..##.", "..##.", "....."),
    ";": (".....", "..##.", "..##.", ".....", "..##.", "..#..", ".#..."),
    "<": ("....#", "...#.", "..#..", ".#...", "..#..", "...#.", "....#"),
    "=": (".....", ".....", "#####", ".....", "#####", ".....", "....."),
    ">": ("#....", ".#...", "..#..", "...#.", "..#..", ".#...", "#...."),
    "?": (".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."),
    "@": (".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".###."),
    "A": (".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"),
    "B": ("####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."),
    "C": (".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."),
    "D": ("###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."),
    "E": ("#####", "#....", "#....", "####.", "#....", "#....", "#####"),
    "F": ("#####", "#....", "#....", "####.", "#....", "#....", "#...."),
    "G": (".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."),
    "H": ("#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"),
    "I": (".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."),
    "J": ("..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."),
    "K": ("#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"),
    "L": ("#....", "#....", "#....", "#....", "#....", "#....", "#####"),
    "M": ("#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"),
    "N": ("#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"),
    "O": (".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "P": ("####.", "#...#", "#...#", "####.", "#....", "#....", "#...."),
    "Q": (".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"),
    "R": ("####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"),
    "S": (".####", "#....", "#....", ".###.", "....#", "....#", "####."),
    "T": ("#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."),
    "U": ("#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "V": ("#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."),
    "W": ("#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"),
    "X": ("#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"),
    "Y": ("#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."),
    "Z": ("#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"),
    "[": ("..###", "..#..", "..#..", "..#..", "..#..", "..#..", "..###"),
    "\\": ("#....", "#....", ".#...", "..#..", "...#.", "....#", "....#"),
    "]": ("###..", "..#..", "..#..", "..#..", "..#..", "..#..", "###.."),
    "^": ("..#..", ".#.#.", "#...#", ".....", ".....", ".....", "....."),
    "_": (".....", ".....", ".....", ".....", ".....", ".....", "#####"),
    "`": (".#...", "..#..", ".....", ".....", ".....", ".....", "....."),
    "{": ("...##", "..#..", "..#..", ".#...", "..#..", "..#..", "...##"),
    "|": ("..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."),
    "}": ("##...", "..#..", "..#..", "...#.", "..#..", "..#..", "##..."),
    "~": (".....", ".....", ".##.#", "#.##.", ".....", ".....", "....."),
}

# 小写按大写渲染（5x7 网格分辨不出小写），无字形的一律按 '?' 处理
for _c in "abcdefghijklmnopqrstuvwxyz":
    GLYPHS[_c] = GLYPHS[_c.upper()]


def glyph_rows(ch):
    art = GLYPHS.get(ch)
    if art is None:
        art = GLYPHS["?"]
    rows = []
    for line in art:
        mask = 0
        for x, px in enumerate(line):
            if px == "#":
                mask |= 1 << x
        rows.append(mask)
    return rows


def show_art(ch):
    art = GLYPHS.get(ch) or GLYPHS["?"]
    print(f"--- {ch!r} ---")
    for line in art:
        print("   " + line)


def fmt_row(masks):
    return ", ".join(f"0x{m:02X}" for m in masks)


def gen_c():
    lines = []
    lines.append("/**")
    lines.append("  * @file    Font.c")
    lines.append("  * @brief   ASCII 点阵字模（5x7，手写点阵）")
    lines.append("  *")
    lines.append("  * 字符集   : ASCII 0x20 ~ 0x7E，共 95 个")
    lines.append(f"  * 字模规格 : 字形 {GLYPH_W}x{GLYPH_H}，单元 {CELL_W}x{CELL_H}（右侧 1 px、下方 2 px 间隔）")
    lines.append(f"  * 大字号   : {CELL_W * BIG_SCALE}x{CELL_H * BIG_SCALE}，由本表 {BIG_SCALE} 倍整数放大，不额外占 Flash")
    lines.append("  * 小写字母 : 按大写渲染（5x7 网格下小写无法分辨）")
    lines.append("  *")
    lines.append("  * 存储约定：每行一个 uint8_t，bit n 对应从左往右第 n 列，1 = 点亮。")
    lines.append("  *           驱动只通过 Font_GetRow() 取数，不感知具体位宽。")
    lines.append("  *")
    lines.append("  * 本文件由 tools/gen_font.py 生成，改字形请改脚本里的 GLYPHS 表后重新生成。")
    lines.append("  */")
    lines.append("")
    lines.append('#include "Font.h"')
    lines.append("")
    lines.append("#define FONT_COUNT        95U")
    lines.append(f"#define FONT_INDEX_BASE   0x{ASCII_FIRST:02X}U")
    lines.append(f"#define FONT_INDEX_LAST   0x{ASCII_LAST:02X}U")
    lines.append("")
    lines.append(f"static const uint8_t s_font_{GLYPH_W}x{GLYPH_H}[FONT_COUNT][{GLYPH_H}] = {{")
    for code in range(ASCII_FIRST, ASCII_LAST + 1):
        ch = chr(code)
        rows = glyph_rows(ch)
        tag = "<sp>" if ch == " " else ch
        lines.append(f"    {{ {fmt_row(rows)} }}, /* 0x{code:02X}  {tag} */")
    lines.append("};")
    lines.append("")
    lines.append("""
/* 字符 -> 表下标；不可显示字符统一按 '?' 处理 */
static uint8_t Font_Index(char ch)
{
    uint8_t code = (uint8_t)ch;

    if ((code < FONT_INDEX_BASE) || (code > FONT_INDEX_LAST))
    {
        code = (uint8_t)'?';
    }
    return (uint8_t)(code - FONT_INDEX_BASE);
}

/* 一行的掩码横向放大一倍：0b00101 -> 0b0000110011 */
static uint16_t Font_Expand2(uint8_t mask)
{
    uint16_t out = 0U;
    uint8_t  i;

    for (i = 0U; i < 8U; i++)
    {
        if ((mask & (uint8_t)(1U << i)) != 0U)
        {
            out |= (uint16_t)(3U << (i * 2U));
        }
    }
    return out;
}

uint8_t Font_Width(Font_Id id)
{
    return (id == FONT_BIG) ? (uint8_t)FONT_BIG_WIDTH : (uint8_t)FONT_SMALL_WIDTH;
}

uint8_t Font_Height(Font_Id id)
{
    return (id == FONT_BIG) ? (uint8_t)FONT_BIG_HEIGHT : (uint8_t)FONT_SMALL_HEIGHT;
}

uint16_t Font_GetRow(Font_Id id, char ch, uint8_t row)
{
    uint8_t index = Font_Index(ch);
    uint8_t src_row;

    if (id == FONT_BIG)
    {
        if (row >= (uint8_t)FONT_BIG_HEIGHT)
        {
            return 0U;
        }
        src_row = (uint8_t)(row / 2U);
        if (src_row >= (uint8_t)FONT_GLYPH_HEIGHT)
        {
            return 0U;
        }
        return Font_Expand2(s_font_5x7[index][src_row]);
    }

    if (row >= (uint8_t)FONT_SMALL_HEIGHT)
    {
        return 0U;
    }
    if (row >= (uint8_t)FONT_GLYPH_HEIGHT)
    {
        return 0U;
    }
    return (uint16_t)s_font_5x7[index][row];
}

uint16_t Font_TextWidth(Font_Id id, const char *text)
{
    uint16_t n = 0U;

    if (text == 0)
    {
        return 0U;
    }
    while (*text != '\\0')
    {
        n++;
        text++;
    }
    return (uint16_t)(n * Font_Width(id));
}
""")
    return "\n".join(lines)


def gen_h():
    return f"""/**
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
  *   FONT_SMALL  {CELL_W}x{CELL_H}   字符区 {GLYPH_W}x{GLYPH_H} + 右侧 1 px、下方 2 px 间隔
  *   FONT_BIG    {CELL_W * BIG_SCALE}x{CELL_H * BIG_SCALE}  由同一张表整数 {BIG_SCALE} 倍放大，不额外占 Flash
  *
  * 字符集约定
  * ----------
  * 支持 ASCII 0x{ASCII_FIRST:02X}~0x{ASCII_LAST:02X}。小写字母按大写渲染，
  * 其余不可显示字符一律显示为 '?'。
  */

#ifndef __FONT_H
#define __FONT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {{
#endif

/* ------------------------------------------------------------------ */
/* 字号                                                                */
/* ------------------------------------------------------------------ */
typedef enum
{{
    FONT_SMALL = 0,   /* {CELL_W}x{CELL_H} px：状态栏 / 参数栏 / 菜单列表 */
    FONT_BIG   = 1    /* {CELL_W * BIG_SCALE}x{CELL_H * BIG_SCALE} px：大号读数 / 标题（{BIG_SCALE} 倍整数放大） */
}} Font_Id;

/* ------------------------------------------------------------------ */
/* 尺寸（含间隔）                                                      */
/* ------------------------------------------------------------------ */
#define FONT_SMALL_WIDTH    {CELL_W}U
#define FONT_SMALL_HEIGHT   {CELL_H}U
#define FONT_BIG_WIDTH      (FONT_SMALL_WIDTH  * {BIG_SCALE}U)
#define FONT_BIG_HEIGHT     (FONT_SMALL_HEIGHT * {BIG_SCALE}U)

/* 单元内真正有点阵的区域（不含间隔） */
#define FONT_GLYPH_WIDTH    {GLYPH_W}U
#define FONT_GLYPH_HEIGHT   {GLYPH_H}U

/* ------------------------------------------------------------------ */
/* 字符集范围                                                          */
/* ------------------------------------------------------------------ */
#define FONT_ASCII_FIRST    0x{ASCII_FIRST:02X}U
#define FONT_ASCII_LAST     0x{ASCII_LAST:02X}U

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
  * @param  ch  字符（ASCII 0x{ASCII_FIRST:02X}~0x{ASCII_LAST:02X}）
  * @param  row 行号，0 = 最上面一行；越界返回 0
  * @retval 位掩码，bit n 对应从左往右第 n 列，1 = 该像素点亮
  */
uint16_t Font_GetRow(Font_Id id, char ch, uint8_t row);

/**
  * @brief  字符串在指定字号下的像素宽度（不含结尾 '\\0'）
  * @param  id    字号
  * @param  text  以 '\\0' 结尾的字符串
  * @retval 像素宽度
  */
uint16_t Font_TextWidth(Font_Id id, const char *text);

#ifdef __cplusplus
}}
#endif

#endif /* __FONT_H */
"""


def main():
    ap = argparse.ArgumentParser(description="生成 LCD 5x7 点阵字模")
    ap.add_argument("--preview", type=str, default=None, help="只预览这些字符，不写文件")
    args = ap.parse_args()

    if args.preview:
        print(f"# cell={CELL_W}x{CELL_H} glyph={GLYPH_W}x{GLYPH_H}")
        for ch in args.preview:
            show_art(ch)
        return 0

    missing = [c for c in map(chr, range(ASCII_FIRST, ASCII_LAST + 1)) if c not in GLYPHS]
    if missing:
        print("ERROR 缺少字形: " + ", ".join(repr(c) for c in missing))
        return 1

    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write(gen_c())
    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write(gen_h())

    print(f"OK 字形 {GLYPH_W}x{GLYPH_H} / 单元 {CELL_W}x{CELL_H} / 大字 {CELL_W*BIG_SCALE}x{CELL_H*BIG_SCALE}")
    print(f"   {OUT_C}")
    print(f"   {OUT_H}")
    print(f"   表大小 {95 * GLYPH_H} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
