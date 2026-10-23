#!/usr/bin/env python3
"""生成 256 点 FFT 用的定点表（无浮点库依赖）。

输出：User/Algorithm/Src/FftTables.h
  - k_fft_hann_q15[256]   Hann 窗，Q15（32767 = 1.0）
  - k_fft_cos_q15[128]    旋转因子 cos(2*pi*m/256)，Q15
  - k_fft_sin_q15[128]    旋转因子 sin(2*pi*m/256)，Q15（0..pi，恒 >= 0）
  - k_fft_log2_q8[256]    log2(1 + i/256)，Q8（用于把功率换算成 dB）

公式与 Fft.c 里的用法一一对应，改动任何一边都要重新生成。
用法：python tools/gen_fft_tables.py
"""

import math
import os

SIZE = 256
HALF = SIZE // 2
Q15 = 32767


def format_values(values, per_line=8, indent="    "):
    lines = []
    for i in range(0, len(values), per_line):
        chunk = ", ".join(str(v) for v in values[i:i + per_line])
        lines.append(indent + chunk + ",")
    return "\n".join(lines)


def main():
    hann = [
        int(round(Q15 * (0.5 - 0.5 * math.cos(2.0 * math.pi * n / SIZE))))
        for n in range(SIZE)
    ]
    hann = [max(0, min(Q15, v)) for v in hann]

    cos_tab = [
        int(round(Q15 * math.cos(2.0 * math.pi * m / SIZE))) for m in range(HALF)
    ]
    sin_tab = [
        int(round(Q15 * math.sin(2.0 * math.pi * m / SIZE))) for m in range(HALF)
    ]

    log2_tab = [
        int(round(256.0 * math.log2(1.0 + i / 256.0))) for i in range(256)
    ]
    log2_tab = [max(0, min(255, v)) for v in log2_tab]

    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.abspath(
        os.path.join(here, "..", "User", "Algorithm", "Src", "FftTables.h")
    )

    text = []
    text.append("/**")
    text.append("  * @file    FftTables.h")
    text.append("  * @brief   FFT 定点查表（由 tools/gen_fft_tables.py 生成，不要手改）")
    text.append("  *")
    text.append("  * 只被 Fft.c 包含。全部是 Q15 / Q8 整数，目标板不需要浮点库。")
    text.append("  */")
    text.append("")
    text.append("#ifndef __FFT_TABLES_H")
    text.append("#define __FFT_TABLES_H")
    text.append("")
    text.append("#include <stdint.h>")
    text.append("")
    text.append("/* Hann 窗，Q15：w[n] = 32767 * (0.5 - 0.5*cos(2*pi*n/256)) */")
    text.append("static const uint16_t k_fft_hann_q15[256] =")
    text.append("{")
    text.append(format_values(hann))
    text.append("};")
    text.append("")
    text.append("/* 旋转因子，Q15：cos(2*pi*m/256) / sin(2*pi*m/256)，m = 0..127 */")
    text.append("static const int16_t k_fft_cos_q15[128] =")
    text.append("{")
    text.append(format_values(cos_tab))
    text.append("};")
    text.append("")
    text.append("static const int16_t k_fft_sin_q15[128] =")
    text.append("{")
    text.append(format_values(sin_tab))
    text.append("};")
    text.append("")
    text.append("/* log2(1 + i/256) 的 Q8 值：0..255，用于整数 dB 换算 */")
    text.append("static const uint8_t k_fft_log2_q8[256] =")
    text.append("{")
    text.append(format_values(log2_tab))
    text.append("};")
    text.append("")
    text.append("#endif /* __FFT_TABLES_H */")
    text.append("")

    with open(out, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(text))

    print("wrote", out)


if __name__ == "__main__":
    main()
