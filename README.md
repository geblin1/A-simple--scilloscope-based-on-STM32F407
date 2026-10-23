# 基于 STM32F407ZGT6 的简易示波器

一个在面包板上搭出来的双通道数字示波器：**STM32F407ZGT6 + ST7789 320×240 SPI 屏**，
采用 **FreeRTOS 三任务**结构，带边沿触发 / 预触发、256 点 FFT 频谱、分通道 V/div、
逐通道两点标定（掉电保存）和增量刷新（典型 26~43 fps）。

> 这是个人学习 / 练手项目，不是产品，也不是高精度仪器。硬件是面包板原型，
> 模拟前端需要手工插接；软件已在真实硬件上运行过触发、AUTO 设置、标定流程等核心功能，
> 较新的功能（FFT、分通道 V/div 等）请按文档自行复测。

---

## 目录

- [主要特性](#主要特性)
- [硬件组成](#硬件组成)
- [软件架构](#软件架构)
- [快速开始](#快速开始)
- [操作速查](#操作速查)
- [显示与测量说明](#显示与测量说明)
- [资源占用与性能](#资源占用与性能)
- [文档索引](#文档索引)
- [目录结构](#目录结构)
- [已知限制 / 未实现](#已知限制--未实现)
- [许可证](#许可证)

---

## 主要特性

- **双通道同步采样**：ADC1（PA0）+ ADC2（PA1）规则同步，TIM2 TRGO 硬件触发，DMA2_Stream0 搬运，
  两路无通道间时差；采样率 1 kSa/s ~ 1 MSa/s。
- **640 点触发采集**：显示窗 320 点 + 预触发 160 点 + 后触发 160 点，触发点固定对齐到屏幕中线。
- **边沿触发**：触发源 CH1/CH2、上升沿/下降沿/OFF，迟滞 `V/div ÷ 20` 抗噪；
  触发点做**亚采样插值对齐**，把触发时刻对齐到采样点之间的真实穿越处，
  帧间相位抖动远小于 1 个采样点，不会破坏增量刷新；
  找不到触发点时回退为自由滚动（红色虚线提示），不黑屏、不冻屏。
- **触发电平 AUTO**：电平落在信号范围外时自动改用信号中点重试，并写回菜单显示。
- **AUTO 自动设置**：一键按显示通道自动选 V/div、按信号周期自动选 Time/div（多帧收敛，避免削顶/判错）。
- **FFT 频谱**：256 点整数 radix-2 FFT（先减均值去直流 + Hann 窗），128 条谱线，
  横轴 8 格 = 0…采样率/2，纵轴 0…−60 dB（相对最强谱线）。
- **分通道 V/div**：两路各自 7 档（100 mV ~ 10 V/div）独立设置，状态栏并列显示两路值与两个量程字母。
- **Time/div**：12 档（20 µs ~ 100 ms，1-2-5 步进），`采样率 = 40 / Time/div`，超限自动钳位并显示 `LIMIT`。
- **逐通道 × 逐量程两点标定**：接 GND 记零点、接 3.3 V 记满点，4 套参数存 flash 扇区 11，
  掉电保存、上电校验，旧格式自动迁移；量程错配时拒绝标定。
- **纯整数测量**：Vpp、真有效值 RMS、直流均值、频率、周期（带滞回的过零检测）。
- **增量刷新**：脏行位图 + 行模板 + FNV-1a 行指纹，稳定触发时只刷变化的几十行。
- **FreeRTOS 三任务**：采集 / 界面 / 监控；采集与刷屏用总线互斥串行化，采样期间不刷屏
  （SPI 电流尖峰会耦合进 ADC 输入）。
- **设置菜单**：7 行覆盖式弹窗，编码器选行/改值，KEY1 全程切换“当前通道”。
- **串口自检**：内置 ADC 自检 / 噪声测试模式（USART1 115200 8N1）。

---

## 硬件组成

| 部件 | 型号 / 说明 |
| :--- | :--- |
| 主控板 | STM32F407ZGT6 核心板（168 MHz，1 MB Flash / 128 KB SRAM） |
| 显示屏 | 2.4" ST7789 IPS，240×320 面板，SPI1 横屏使用 320×240，10.5 Mbit/s |
| 模拟前端 | 面包板搭建：1.65 V 偏置 + 可切换衰减（1 kΩ/1 kΩ 分压 + Rs 衰减电阻）+ 100 nF 去耦 + 肖特基钳位 |
| 输入耦合 | 跳线甲：`IN`↔`DC` 直流耦合 / `AC`↔`DC` 交流耦合（串 100 nF） |
| 输入量程 | 跳线乙：插 `MID` = 小量程；不插 = 大量程（需与该路 V/div 对应） |
| 人机交互 | KEY1~KEY4 两脚按键（PE2~PE5）、两个 EC11 旋转编码器、1 个运行指示灯（PC8） |
| 调试 | SWD（PA13/PA14）、USART1（PA9/PA10，115200） |

### 引脚分配

| 模块 | 信号 | 引脚 | 模式 |
| :--- | :--- | :--- | :--- |
| CH1 输入 | ADC1_IN0 | PA0 | 模拟输入 |
| CH2 输入 | ADC2_IN1 | PA1 | 模拟输入 |
| ADC 采样触发 | TIM2_TRGO | 内部 | 定时器更新事件 → ADC1 外部触发 |
| LCD | SCK / MOSI | PA5 / PA7 | SPI1 复用推挽 |
| LCD | CS / DC / RES / BLK | PA4 / PB0 / PB1 / PB2 | GPIO 推挽输出（BLK 高电平亮） |
| 编码器1（垂直） | A / B | PC6 / PC7 | TIM8_CH1/2 硬件编码器 x4 |
| 编码器2（水平） | A / B | PB6 / PB7 | TIM4_CH1/2 硬件编码器 x4 |
| 按键 | KEY1~KEY4 | PE2 / PE3 / PE4 / PE5 | GPIO 输入（内部上拉，低有效） |
| 运行灯 | LED | PC8 | GPIO 推挽输出（高电平亮） |
| 调试串口 | TX / RX | PA9 / PA10 | USART1 复用，115200 8N1 |
| 调试接口 | SWDIO / SWCLK | PA13 / PA14 | SWD |

时钟：HCLK = 168 MHz，APB1 = 42 MHz，APB2 = 84 MHz，ADCCLK = 21 MHz；HAL 毫秒时基用 **TIM7**（非 SysTick）。

### 模拟前端

前端电路（节点定义、固定接线、四种“耦合 × 量程”组合、元件清单、接线检查表）见
[FRONTEND_BIAS_DESIGN.md](FRONTEND_BIAS_DESIGN.md)。要点：

- ADC 引脚是三支路节点：1 kΩ 到 3.3 V、1 kΩ 到 GND、Rs 到信号源；
  `V_pin = 1.65 V + (V_in − 1.65 V) × g`，`g = 500 / (500 + Rs)`。
- 大量程 Rs = 10 kΩ：输入范围约 **−33.0 … +36.3 V**，16.9 mV/LSB；
  小量程 Rs = 909 Ω：输入范围约 **−3.0 … +6.3 V**，2.27 mV/LSB。
- 交流耦合高通拐点约 **152 Hz（大量程）/ 1130 Hz（小量程）**。
- **不要直接测市电或高压**；输入不得超过前端钳位允许范围。面包板前端仅用于低压学习实验。

---

## 软件架构

```
┌─────────────────────────────────────────────────────────────┐
│  App 层      App_Scope.c / ScopeMenu.c / App_Rtos.c          │
│              采集调度、菜单、标定流程、AUTO、FFT 显示合成      │
├─────────────────────────────────────────────────────────────┤
│  View 层     View.c / WaveView.c / StatusBar.c / ParamBar.c  │
│              只认 "mV / 采样率 / 波形数组"，不含 HAL 头文件    │
├─────────────────────────────────────────────────────────────┤
│  Algorithm   Measure.c / Trigger.c / Fft.c                   │
│              纯整数测量、触发查找、256 点 FFT，无硬件访问      │
├─────────────────────────────────────────────────────────────┤
│  BSP 层      ADC_Drv.c / Frontend.c / Lcd.c / Font.c /        │
│              EncoderDrv.c / Key.c / LED.c / Retarget.c       │
│              唯一接触 HAL 与寄存器的一层                      │
└─────────────────────────────────────────────────────────────┘
```

| 模块 | 职责 |
| :--- | :--- |
| `User/App/Src/App_Scope.c` | 应用状态机：采集链路、触发/AUTO、FFT 显示、标定流程、界面刷新调度 |
| `User/App/Src/ScopeMenu.c` | 220×170 覆盖式设置菜单（7 行，编码器操作） |
| `User/App/Src/App_Rtos.c` | FreeRTOS 三任务创建与主循环（acq / ui / mon） |
| `User/View/Src/WaveView.c` | 波形区增量绘制（脏行、行模板、行指纹、触发电平虚线） |
| `User/View/Src/StatusBar.c` `ParamBar.c` | 状态栏（两路 V/div、量程字母、采样率、Time/div）与参数栏（F/T/Vpp/RMS/DC） |
| `User/Algorithm/Src/Measure.c` | 过零测频、Vpp、RMS、均值（全整数） |
| `User/Algorithm/Src/Trigger.c` | 带迟滞的边沿查找 + 亚采样位置 `frac_q16` |
| `User/Algorithm/Src/Fft.c` | 256 点整数 FFT、Hann 窗、整数 log2 → dB |
| `User/BSP/Src/ADC_Drv.c` | ADC1+ADC2 同步、TIM2 TRGO、DMA2_Stream0、640 点采集 |
| `User/BSP/Src/Frontend.c` | 逐通道 × 逐量程标定表、raw→mV、flash 扇区 11 读写 |
| `User/BSP/Src/Lcd.c` `Font.c` | ST7789 驱动（窗口批量写、RGB565）、5×7 手写点阵字库 |

### FreeRTOS 任务

| 任务 | 优先级 | 栈 | 周期 | 工作内容 |
| :--- | :--- | :--- | :--- | :--- |
| `acq` | idle+4 | 1.0 KB | 20 ms 起 | 采一帧（640 点）→ 触发查找 → 两路测量 → 发布 → 让出 1 tick |
| `ui` | idle+3 | 2.5 KB | 10 ms | 读按键/编码器 → 改状态 → 取新帧 → 增量重绘 |
| `mon` | idle+1 | 0.5 KB | 500 ms | 心跳、堆余量、栈高水位监控 |

采集与刷屏共用一把总线互斥量：采集期间不刷屏（避免 SPI 电流尖峰耦合进采样窗口）；
界面任务取锁有 20 ms 上限，拿不到锁就保留“欠一帧”标志，下一轮再画。

---

## 快速开始

### 1. 准备工具链

- CMake ≥ 3.22
- Ninja
- `arm-none-eabi-gcc`（实测 14.3.1；需在 PATH 中，或按 `cmake/gcc-arm-none-eabi.cmake` 里的说明自动查找 STM32Cube 工具链）
- 烧录工具：ST-Link + STM32CubeProgrammer（或 STM32CubeIDE）

### 2. 编译

```bash
# 在仓库根目录
cmake --preset Debug
cmake --build --preset Debug
```

或（使用 VS Code 的 STM32Cube 扩展时）直接执行 `cube-cmake --build build/Debug`。

产物：

```
build/Debug/STM32F407ZGT6_Scope_SignalGenerator_Multimeter.elf
build/Debug/STM32F407ZGT6_Scope_SignalGenerator_Multimeter.map
```

Release 构建用 `--preset Release`。编译选项：`-Wall -O0 -g3 -std=gnu11`（Debug）。

### 3. 烧录

```bash
# STM32CubeProgrammer CLI 示例
STM32_Programmer_CLI -c port=SWD -w build/Debug/STM32F407ZGT6_Scope_SignalGenerator_Multimeter.elf -rst
```

或直接用 STM32CubeIDE / ST-Link Utility 打开 ELF 烧录。

### 4. 首次上电

1. 按 [FRONTEND_BIAS_DESIGN.md](FRONTEND_BIAS_DESIGN.md) 接好两路模拟前端（面包板）。
2. 跳线甲按需要的耦合方式插好（`IN`↔`DC` 直流 / `AC`↔`DC` 交流）。
3. 跳线乙按该路 V/div 对应插好（前 4 档小量程插 `MID`，后 3 档大量程不插）。
4. 上电后屏幕显示 `PRESS KEY4 TO RUN`，按 **KEY4** 开始采集。
5. 如果波形不“钉”住：按 **KEY2** 进菜单，把 `Trig level` 或 `Trig src` 调到正确位置；
   或者把光标移到 `Auto set` 行按 **KEY4** 让它自动选档。

### 5. 自检模式（可选）

`Core/Src/main.c` 里有两个编译期开关（默认都为 0）：

- `APP_RUN_SELFCHECK`：裸机 ADC 自检，串口 115200 打印两路 raw / 电压 / 噪声。
- `APP_RUN_NOISETEST`：噪声测试，对比“采集时刷屏 / 不刷屏”的噪声差异。

---

## 操作速查

| 操作 | 主界面 | 设置菜单 |
| :--- | :--- | :--- |
| **KEY1** | 切换“当前通道”：参数栏测量 + 触发源 + 编码器1 的 V/div + 标定目标一起切 | 任何一行都切换当前通道（切换后触发迟滞/电平同步） |
| **KEY2** | 进入设置菜单 | 返回主界面 |
| **KEY3** | 显示模式循环：双路 → 仅 CH1 → 仅 CH2 → FFT 频谱 | 不响应 |
| **KEY4** | RUN / HOLD | `Auto set` 行：执行 AUTO 自动设置；`Cal` 行：启动两点标定 |
| **编码器1** | 调当前通道的 V/div（两路各自独立） | 上下移动光标（7 行循环） |
| **编码器2** | 调 Time/div | 改选中行的值 |

### 设置菜单（7 行，220×170 弹窗）

| 行 | 项目 | 取值 |
| :--- | :--- | :--- |
| 1 | `V/div C1/C2` | 100 / 200 / 500 / 1000 / 2000 / 5000 / 10000 mV，按当前通道，行内并列显示两路值 |
| 2 | `Time/div` | 20 / 50 / 100 / 200 / 500 µs，1 / 2 / 5 / 10 / 20 / 50 / 100 ms |
| 3 | `Trig src` | `CH1` / `CH2` |
| 4 | `Trig edge` | `OFF` / `RISE` / `FALL` |
| 5 | `Trig level` | ±5 格，步进 = 触发通道的 `V/div ÷ 10` |
| 6 | `Auto set` | 按 KEY4：逐显示通道自动选 V/div + 按周期自动选 Time/div |
| 7 | `Cal` | 按 KEY4 启动两点标定（通道由 KEY1 选） |

### 两点标定流程

1. KEY2 进菜单，编码器1 转到 `Cal` 行（行内显示当前要标定的通道，KEY1 可切换）。
2. 按 **KEY4** 启动标定，屏幕出现 `CAL CH1/S GND KEY4  K1:CH K2:ESC` 提示。
3. 探头接 **GND**，再按 **KEY4** → 采样 64 次取平均作为零点（提示变为 `3V3`）。
4. 探头接 **板载 3.3 V**，按 **KEY4** → 采样 64 次作为满点。
5. 自动保存到 flash 扇区 11，显示 `CAL OK SAVED`；标定过程中 KEY2 可随时取消。
6. 标定按“通道 × 量程”共 4 套；换量程/换跳线后各标各的。
7. 提示含义：`CAL FAIL: 3V3/RANGE?` = 码值差不在该量程合理窗口（跳线/档位可能对不上）；
   `CAL FAIL: ADC BUSY` = 采样忙，重试即可。

---

## 显示与测量说明

- **屏幕布局**：状态栏 22 px / 波形区 190 px（8 × 10 网格，每格 40 × 19 px，中线 y = 117）/
  参数栏 28 px。纵向映射 `y = 117 − mv × 19 / V/div`。
- **状态栏**：当前通道、两路 V/div（CH1 色 / CH2 色）、采样率、Time/div、两路量程字母 `S`/`W`、
  RUN/HOLD、`LIMIT`。
- **触发电平标记**：横贯波形区的 2 亮 2 灭虚线（左端带小方块）。
  灰色 = 已锁定，红色 = 未触发（本机面板 R/B 顺序反，实际偏青/蓝）；触发关闭时不显示。
- **参数栏**：`F` 频率、`T` 周期、`Vpp`、`RMS`、`DC`（直流均值）。
- **FFT 模式**：频谱源 = 当前通道（KEY1 选）。
  - 横轴：8 格 = 0 … 采样率/2，每格 = 采样率/16；谱线间隔 = 采样率/256。
  - 纵轴：0 … −60 dB，相对最强谱线；直流分量已先去均值且 bin0 不显示。
  - 状态栏 V/div 位置显示 `FFT`，Time 位置显示“频率/格”。
  - HOLD 后频谱冻结；自由滚动时频谱会随窗口相位轻微变化。
- **测量限制**：幅度 < 60 mV 不测频率（显示 `---`）；频率用带滞回的过零检测，
  门限 = 均值 ± max(幅度/8, 5 mV)。

---

## 资源占用与性能

Debug 构建实测（arm-none-eabi-gcc 14.3.1）：

| 项目 | 占用 | 占比 |
| :--- | ---: | ---: |
| FLASH | **70 088 B** / 1 MB | **6.68 %** |
| RAM | **59 688 B** / 128 KB | **45.54 %** |
| CCMRAM | 0 B | — |

- 采样率：1 kSa/s ~ 1 MSa/s（`ADC_Drv` 对外钳位）。
- 每帧采集：640 点 = 320 显示 + 160 预触发 + 160 后触发；一帧时长 = 640 / 采样率。
- 刷新率：稳定触发时约 **26 ~ 43 fps**（只刷变化行）；自由滚动/未触发时整片重画（46~122 KB/帧，约 50~110 ms）。
- 慢时基：1 kSa/s 时一帧约 640 ms，画面更新慢属正常，按键/编码器仍可操作。

---

## 文档索引

| 文档 | 内容 |
| :--- | :--- |
| [PROJECT_SPEC.md](PROJECT_SPEC.md) | 需求、硬件引脚、前端方案、软件设计、资源预算、功能清单 |
| [CONTROLS.md](CONTROLS.md) | 操作手册：按键/编码器、菜单逐项说明、触发/AUTO/标定/FFT 用法、FAQ |
| [TROUBLESHOOTING.md](TROUBLESHOOTING.md) | 约 2600 行调试记录：问题 1~34、各模块实现方法（LCD / FreeRTOS / 触发 / 调度 / 标定 / FFT / 分通道 V/div） |
| [LCD_UI_DESIGN.md](LCD_UI_DESIGN.md) | 界面布局、菜单、交互映射的设计稿与“实际实现”注记 |
| [FRONTEND_BIAS_DESIGN.md](FRONTEND_BIAS_DESIGN.md) | 模拟前端：偏置原理、跳线组合、元件清单、接线检查表 |

---

## 目录结构

```
.
├── CMakeLists.txt / CMakePresets.json   # CMake 工程与 Debug/Release 预设
├── STM32F407xx_FLASH.ld                 # 链接脚本
├── startup_stm32f407xx.s                # 启动文件
├── 基于STM32F407ZGT6的简易示波器.ioc      # CubeMX 配置
├── cmake/                               # 工具链文件 + CubeMX 源清单
├── Core/                                # CubeMX 生成的初始化（main.c / adc.c / tim.c / ...）
├── Drivers/                             # STM32F4 HAL + CMSIS
├── ThirdParty/FreeRTOS/                 # FreeRTOS 内核 + Cortex-M4F 移植
├── User/
│   ├── App/                             # App_Scope / ScopeMenu / App_Rtos / ADC_NoiseTest
│   ├── View/                            # View / WaveView / StatusBar / ParamBar
│   ├── Algorithm/                       # Measure / Trigger / Fft（纯整数，无 HAL）
│   └── BSP/                             # ADC_Drv / Frontend / Lcd / Font / Key / EncoderDrv ...
├── tools/                               # gen_font.py / calc_frontend.py / gen_fft_tables.py
├── PROJECT_SPEC.md / CONTROLS.md / TROUBLESHOOTING.md / ...
└── build/                               # 构建产物（已 gitignore）
```

构建时 `User/` 下的源文件与头文件目录会被 CMake 自动 glob（`CONFIGURE_DEPENDS`），
新增文件不需要改 CMakeLists。

---

## 已知限制 / 未实现

- **AC/DC 耦合方式**由跳线甲决定，固件读不到，状态栏不显示；自动识别（加检测线）评估后决定不实现。
- **前端量程**也只能靠跳线乙与 V/div 手动对应，状态栏用两个 `S`/`W` 字母提示。
- **FFT 是整数近似**：Hann 窗、相对 dB、直流不显示，不是校准过的频谱仪；幅度绝对值仅供参考。
- **慢时基帧率低**：640 点一帧，1 kSa/s 时约 640 ms/帧。
- **模拟前端是面包板原型**：输入保护有限，请勿测量市电/高压；最大输入范围约 −33 … +36 V（大量程）。
- **分通道 V/div、FFT 等较新功能**主要经过 ARM 构建 + 代码审查与部分实机验证，上板前建议按
  `CONTROLS.md` / `TROUBLESHOOTING.md` 的“验证状态”逐项自测。
- 未实现：AC/DC 自动识别、频谱峰值自动测量（可后续加）、串口波形导出。

---

## 许可证

本项目暂未指定开源许可证。上传到 GitHub 前如需明确授权方式，
建议补充 `LICENSE` 文件（例如 MIT 或 Apache-2.0）。

---

## 致谢

- STMicroelectronics：STM32F4 HAL 驱动与 CubeMX 工程骨架
- FreeRTOS：实时内核（`ThirdParty/FreeRTOS`）
- 以及所有公开的 ST7789 驱动与数字示波器实现参考资料
