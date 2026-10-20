#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""模拟前端（偏置版）参数计算器。

电路（每通道一套）：
    3.3V --[R_UP]--+--[R_DOWN]-- GND
                   |
                   +--[Rs]-- 信号源 Vin

其中 Rs 是"信号源到 ADC 孔"的等效串联电阻：
    跳线乙不插（大量程）：Rs = R_10K                     = 10 kOhm
    跳线乙插 DC-MID（小量程）：Rs = R_10K // R_MID(1k)   = 909 Ohm

节点方程（KCL）：
    (Vn - VCC)/R_UP + Vn/R_DOWN + (Vn - Vin)/Rs = 0

先求分压器的戴维南等效：
    Vth = VCC * R_DOWN / (R_UP + R_DOWN)
    Rth = R_UP // R_DOWN
则：
    Vn  = Vth + (Vin - Vth) * Rth / (Rth + Rs)
    增益 g = Rth / (Rth + Rs)
    零点（Vin = 0 时的引脚电压）= Vth * Rs / (Rth + Rs) = Vth * (1 - g)

反解输入范围（Vn 从 0 到 VCC）：
    Vin_min = -Vth * (1/g - 1)
    Vin_max = +Vth * (1/g + 1)
    即：范围宽度 = VCC / g，中心始终在 +Vth。
"""

VCC = 3.3           # V
ADC_FULL = 4095.0   # 12 bit
R_UP = 1000.0       # 新增上偏置
R_DOWN = 1000.0     # 原有下拉
R_10K = 10000.0     # 原有量程电阻
R_MID = 1000.0      # 新增小档串阻
C_NODE = 100e-9     # ADC 孔上的 100nF
C_AC = 100e-9       # IN 与 AC 之间的 100nF


def analyze(name, rs):
    vth = VCC * R_DOWN / (R_UP + R_DOWN)
    rth = R_UP * R_DOWN / (R_UP + R_DOWN)
    g = rth / (rth + rs)
    zero = vth * (1.0 - g)
    vin_min = -vth * (1.0 / g - 1.0)
    vin_max = vth * (1.0 / g + 1.0)
    lsb_mv = (VCC / ADC_FULL / g) * 1000.0
    zin = rs + rth
    r_par = rth * rs / (rth + rs)
    lp_hz = 1.0 / (2.0 * 3.141592653589793 * r_par * C_NODE)
    hp_hz = 1.0 / (2.0 * 3.141592653589793 * zin * C_AC)

    def raw(v):
        return int(round(v / VCC * ADC_FULL))

    return {
        "name": name, "rs": rs, "g": g, "zero": zero,
        "zero_mv": zero * 1000.0,
        "raw_zero": raw(zero),
        "vin_min": vin_min, "vin_max": vin_max,
        "lsb_mv": lsb_mv, "zin": zin, "lp_hz": lp_hz, "hp_hz": hp_hz,
        "raw_float": raw(vth),          # 输入悬空：串联电阻无电流 -> Vn = Vth
        "raw_1v": raw(zero + 1.0 * g),
        "raw_3v3": raw(zero + 3.3 * g),
        "raw_vcc": raw(VCC),
        "raw_0": raw(0.0),
    }


def main():
    vth = VCC * R_DOWN / (R_UP + R_DOWN)
    rth = R_UP * R_DOWN / (R_UP + R_DOWN)
    print("bias Thevenin : Vth = %.1f mV, Rth = %.1f ohm" % (vth * 1000, rth))
    print("")

    wide = analyze("WIDE  (jumper2 open)", R_10K)
    small = analyze("SMALL (jumper2 MID )", R_10K * R_MID / (R_10K + R_MID))

    hdr = "%-22s %10s %10s" % ("item", "WIDE", "SMALL")
    print(hdr)
    print("-" * len(hdr))
    rows = [
        ("Rs (ohm)", "rs", "%.1f"),
        ("gain (Vn/Vin)", "g", "1/%.3f"),
        ("zero pin (mV)", "zero_mv", "%.0f"),
        ("zero raw", "raw_zero", "%d"),
        ("Vin min (V)", "vin_min", "%+.2f"),
        ("Vin max (V)", "vin_max", "%+.2f"),
        ("LSB at input (mV)", "lsb_mv", "%.2f"),
        ("Zin (ohm)", "zin", "%.0f"),
        ("pin LP (Hz)", "lp_hz", "%.0f"),
        ("AC HP (Hz)", "hp_hz", "%.0f"),
    ]
    for label, key, fmt in rows:
        a = fmt % (1.0 / wide[key] if key == "g" else wide[key])
        b = fmt % (1.0 / small[key] if key == "g" else small[key])
        if key == "g":
            a = "1/%.3f" % (1.0 / wide[key])
            b = "1/%.3f" % (1.0 / small[key])
        print("%-22s %10s %10s" % (label, a, b))

    print("")
    print("raw value self-check")
    print("%-22s %10s %10s" % ("input", "WIDE", "SMALL"))
    print("-" * 44)
    for label, key in [("floating (open)", "raw_float"), ("GND (0 V)", "raw_zero"),
                       ("1.0 V", "raw_1v"), ("3.3 V", "raw_3v3"),
                       ("pin at 0 V", "raw_0"), ("pin at VCC", "raw_vcc")]:
        print("%-22s %10d %10d" % (label, wide[key], small[key]))

    print("")
    print("measured on real hardware (CH1, input clipped to GND):")
    print("  WIDE  1941   -> model %d  (err %+.1f%%)" %
          (wide["raw_zero"], (1941 - wide["raw_zero"]) / wide["raw_zero"] * 100))
    print("  SMALL 1320   -> model %d  (err %+.1f%%)" %
          (small["raw_zero"], (1320 - small["raw_zero"]) / small["raw_zero"] * 100))


if __name__ == "__main__":
    main()
