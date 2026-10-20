# 调试问题复盘（Troubleshooting Log）

> 项目：基于 STM32F407ZGT6 的简易示波器（ADC 双通道同步采样 + 信号发生器 + 万用表）
> 工具链：STM32CubeMX 生成 + CMake/Ninja + arm-none-eabi-gcc 14.3.1 + HAL + FreeRTOS
> 本文记录开发过程中实际踩到的坑：**现象 → 根因 → 定位过程 → 解决 → 验证**，
> 便于日后复盘。每条都对应真实发生过的故障，不是假设。

---

## 目录速查

| 编号 | 问题 | 一句话根因 |
|---|---|---|
| 1 | `User/` 下新增文件不进编译 | 根 `CMakeLists.txt` 没有收集该目录的源文件 |
| 2 | `printf` 报 implicit declaration | 缺 `<stdio.h>`，C99 起隐式声明是错误 |
| 3 | `printf` 没有输出到串口 | newlib 的 `_write` 未重定向 |
| 4 | 编码器读数恒为 0 | TIM8 从未启动，且不是编码器模式（SMS=0） |
| 5 | 编码器启动代码放错函数 | 误写进 `MX_TIM4_Init()`，且 TIM4 编码器2 未启动 |
| 6 | TIM2 触发永远不来 | CubeMX 给 32 位定时器填了 `Period=4294967295` |
| 7 | 硬件触发后软件启动失效 | `EXTEN≠0` 时硬件忽略 `SWSTART` |
| 8 | ADC 两路读数恒为 0 | ADC2 的 ADON 从未置位 |
| 9 | CH2 数值永远不变（零噪声死数） | 从器件 ADC2 的 `EXTEN=00`，没有触发边沿，只转换了第一次 |
| 10 | DMA 只搬了 1 个点 | 多 ADC 模式的 `HAL` 不置 `CR2.DMA`；`CCR.DDS` 被覆盖 |
| 11 | 按键只亮灭、不闪烁 | 代码里根本没有闪烁逻辑，且按键只在松手沿上报一次 |
| 12 | 串口助手收不到数据 | 板载 CH340 未接到 MCU，需外部跳线 |
| 13 | 编码器转一格数值跳 4 | TIM8/TIM4 是 x4 计数，没折算成"格" |
| 14 | 双通道波形完全重合、`RUN`/`HOLD` 像反了 | 调度器没启动时 `taskENTER_CRITICAL()` 把中断永久关死 |
| 15 | 修好 CH2 后自检报 `READ FAIL` | 把 ADC1 的 `EXTEN` 复制给了 ADC2，`ReadOnce` 只清了主器件的 |
| 16 | 波形在动、`RMS`/`DC` 不动 | 测量被关在"采集成功"分支里，而帧尾 `OVR` 让每帧都判失败 |
| 17 | 状态栏显示成 `Dx1` | `%cx%u` 把一个字符串指针当成 `char` 打印 |
| 18 | 编码器1 只改一个通道的 V/div | 只更新了"当前测量通道"的档位 |
| 19 | 退出菜单后弹窗又出现 | 退出路径仍无条件调用 `App_DrawMenu()` |
| 20 | 进菜单不显示、转一下编码器才出现 | 进入那一帧只重画了波形和条带，没画弹窗 |
| 21 | 面板实际颜色与配置相反（蓝/红 vs 黄/青） | ST7789 的 RGB/BGR 顺序（`MADCTL`）与配置不同 |
| 22 | 帧率只有约 8 fps | 每帧整片重画 166 KB，而 SPI 只有 1.31 MB/s |
| 23 | 零点/增益算错，实测与纸面对不上 | 把 `R_th`(500 Ω) 当成 `R_down`(1000 Ω)，`Rs` 其实与它**并联** |
| 24 | CH2 没搭建时读数 3.27 V，像短路 | 高阻悬空被防过压肖特基的反向漏电流拉高 |

---

## 一、构建与工程配置

### 问题 1：在 `User/` 目录下新增文件不会自动参与编译

**现象**
在 `User/` 下新建 `.c` / `.h` 后，编译既不报错也不链接；`build/Debug/compile_commands.json`
里根本没有 `User/` 下的任何文件（连原有的 `User/BSP/Src/SPI.c` 都没被编译）。

**根因**
CubeMX 生成的 CMake 工程把源文件列表**显式写死**在 `cmake/stm32cubemx/CMakeLists.txt`
（`MX_Application_Src`、`STM32_Drivers_Src`）里，只包含 `Core/Src`、`Drivers/...`。
根 `CMakeLists.txt` 虽然在 `target_sources()` 处留了 `# Add user sources here` 的注释，
但没有任何实际的收集逻辑，`User/` 目录等于被完全忽略。

**解决**
在根 `CMakeLists.txt` 中新增一段（放在 `target_sources()` 注释之后）：

```cmake
set(USER_DIR "${CMAKE_SOURCE_DIR}/User")

# 递归收集源文件；CONFIGURE_DEPENDS 让 Ninja 每次构建都检查目录变化
file(GLOB_RECURSE USER_SOURCES CONFIGURE_DEPENDS
     "${USER_DIR}/*.c" "${USER_DIR}/*.cpp" "${USER_DIR}/*.cc"
     "${USER_DIR}/*.s" "${USER_DIR}/*.S")
list(REMOVE_DUPLICATES USER_SOURCES)   # Windows 文件系统大小写不敏感，*.s 与 *.S 会重复

# 递归收集头文件所在的目录，自动加入 include 路径
file(GLOB_RECURSE USER_HEADERS CONFIGURE_DEPENDS "${USER_DIR}/*.h" "${USER_DIR}/*.hpp")
set(USER_INCLUDE_DIRS "${USER_DIR}")
foreach(_hdr ${USER_HEADERS})
  get_filename_component(_dir "${_hdr}" DIRECTORY)
  list(APPEND USER_INCLUDE_DIRS "${_dir}")
endforeach()
list(REMOVE_DUPLICATES USER_INCLUDE_DIRS)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${USER_SOURCES})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${USER_INCLUDE_DIRS})
```

**关键点：`CONFIGURE_DEPENDS`**
没有它，`file(GLOB)` 只在 configure 阶段求值一次，新增文件必须手动重新 configure。
有了它，CMake 会让构建系统每次构建前检查 glob 结果，发现变化就自动重跑 CMake。

**验证**
新建探针文件 `User/Math/Src/autoglob_probe.c` 后直接 `cmake --build`，输出：

```
-- GLOB mismatch!  The following files were added: + .../User/Math/Src/autoglob_probe.c
[1/2] Re-running CMake...
[2/16] Building C object .../User/Math/Src/autoglob_probe.c.obj
```

`arm-none-eabi-nm` 在该 `.obj` 中确认到 `T autoglob_probe_add`，且
`compile_commands.json` 的编译命令里出现了 `-I.../User/Math/Inc`（证明头文件目录也自动加上了）。
删除探针文件后再构建，会自动从构建中移除。

**经验**
`GLOB` 在 CMake 里通常被认为"不推荐"，但配合 `CONFIGURE_DEPENDS` 后，
对一个需要频繁加文件的业务目录是合理的权衡——**前提是这个目录完全由自己管理，不被代码生成器覆盖**。

---

### 问题 2：主循环里加 `printf` 编译不过

**现象**

```
Core/Src/main.c:109:5: error: implicit declaration of function 'printf' [-Wimplicit-function-declaration]
  109 |     printf("aaa\r\n");
      |     ^~~~~~
note: include '<stdio.h>' or provide a declaration of 'printf'
```

**根因**
没有 `#include <stdio.h>`。从 C99 起，函数未声明就调用属于**错误**（不再是警告）。

**解决**
在 `Core/Src/main.c` 的 `USER CODE BEGIN Includes` 段内加：

```c
#include <stdio.h>
```

放在 `USER CODE` 段内是为了让 CubeMX 重新生成代码时保留。

**经验**
CubeMX 生成的 `main.c` 默认只包含工程自身需要的头文件，标准库头文件要自己加；
而且一定要加在 `USER CODE BEGIN/END` 之间，否则下次生成就被清掉了。

---

### 问题 3：`printf` 重定向到 USART1（PROJECT_SPEC 第 4 项）

**目标**
把 `printf` 重定向到 USART1（115200 8N1），用于输出调试日志与波形数据。

**根因**
STM32 上 `printf` 最终调用 newlib 的 `_write()`，而 CubeMX 生成的
`Core/Src/syscalls.c` 里 `_write` 是 `__attribute__((weak))` 的空实现
（`_write` 内部循环调用同样是 weak 的 `__io_putchar`），默认什么也不发。

**解决**
新建 `User/BSP/Src/Retarget.c`，提供一个**强符号** `_write` 覆盖弱符号：

```c
#include "main.h"
#include "usart.h"

int _write(int file, char *ptr, int len)
{
    int total = len;          /* 关键：先存原始长度 */
    (void)file;

    while (len > 0)
    {
        uint16_t n = (len > 1024) ? (uint16_t)1024 : (uint16_t)len;
        if (HAL_UART_Transmit(&huart1, (uint8_t *)ptr, n, 100U) != HAL_OK)
        {
            return -1;
        }
        ptr += n;
        len -= n;
    }
    return total;             /* 必须返回原始长度 */
}
```

**踩到的两个坑**

1. **返回值语义**：`_write` 必须返回**实际写出的字节数**。最初写成返回递减后的 `len`
   （循环结束时是 0），`printf` 会认为一个字节都没写出去，行为异常。正确做法是先存
   `int total = len;` 再返回 `total`。
2. **分块发送**：`HAL_UART_Transmit` 的长度参数是 `uint16_t`，一次发太多会被截断，
   所以按 1024 字节分块。

**验证（链接期证据）**
在 map 文件里看到两个同名 section，弱符号被丢弃、强符号被链接：

```
.text._write  0x00000000  0x38  ...Core/Src/syscalls.c.obj      ← 被丢弃
.text._write  0x08002ccc  0x68  ...User/BSP/Src/Retarget.c.obj  ← 实际链接
```

ELF 中 `08002ccc T _write`，且 `libc_nano.a(libc_a-stdio.o)` 的 `_write_r` 引用它。

**一个容易被误判的现象**
当程序里**没有任何** `printf` 调用时，`-ffunction-sections -Wl,--gc-sections` 会把
`_write` 一起回收，`nm` 里看不到它。这不是没重定向成功，而是"没人用所以被删了"。
加一句 `printf` 就会重新出现（上面的 map 证据就是这么来的：临时用
`__attribute__((constructor))` 强制拉入一次 printf 验证）。

**经验**
弱符号覆盖（strong-over-weak）是嵌入式里非常常用的扩展手段，但**符号层面的验证必须看
map / nm**，光看"编译通过"说明不了任何问题。

---

## 二、定时器与编码器

### 问题 4：编码器读数恒为 0（串口收到的全是 `00000`）

**现象**
主循环里 `printf("%05d\r\n", Encoder_Get());` 输出恒为 0。

**根因（三层，前两层是致因）**

1. **定时器根本没启动**：全工程没有任何 `HAL_TIM_Encoder_Start` / `HAL_TIM_Base_Start`，
   所以 `TIMx_CR1.CEN = 0`，计数器永远停在 0，`CNT` 读出来就是 0。
2. **TIM8 不是编码器模式**：`Core/Src/tim.c` 里 TIM8 配的是
   `TIM_CLOCKSOURCE_INTERNAL`（`SMCR.SMS = 000`）+ `HAL_TIM_IC_Init()`（输入捕获），
   即使启动了，也只是按 84 MHz 内部时钟计数，跟编码器脉冲毫无关系。
3. **（非致因）** `Encoder.c` 里 `__HAL_TIM_SET_COUNTER(&htim8, 0);` 被注释掉了，
   所以读到的是累计值而不是增量。

**关键 HAL 事实（读源码确认）**
`HAL_TIM_Encoder_Start()`（`stm32f4xx_hal_tim.c:3214`）**只做三件事**：
把通道状态置 BUSY、`TIM_CCxChannelCmd(... CCx_ENABLE)`、`__HAL_TIM_ENABLE()`。
**它不会设置 `SMCR.SMS`** —— 编码器模式必须由 `HAL_TIM_Encoder_Init()` 设置。
而工程里用的是 `HAL_TIM_IC_Init()`，所以必须手动补 `SMS`。

好消息是：CubeMX 已经把通道配成 `CC1S=01 / CC2S=01 + ICF=15` 的输入配置，
这正好满足编码器模式对通道的要求，所以**不需要重跑 `HAL_TIM_Encoder_Init`**。

**解决（改动最小化）**
在 `Core/Src/tim.c` 的 `USER CODE BEGIN TIM8_Init 2` 段内加两行：

```c
htim8.Instance->SMCR |= (TIM_SMCR_SMS_1 | TIM_SMCR_SMS_0);   /* SMS=011 编码器模式3 */
(void)HAL_TIM_Encoder_Start(&htim8, TIM_CHANNEL_ALL);        /* 使能通道 + CEN=1 */
```

不碰 CubeMX、不改 `Encoder.c` 原有的 API（`Encoder_Get()` 保持返回累计值）。

**验证**
反汇编 `MX_TIM8_Init` 尾部：

```
800332a: orr.w r2, r2, #3        ; SMCR |= 0b011
800332e: str   r2, [r3, #8]      ; 写入 TIM8->SMCR
8003334: bl    80007f6 <HAL_TIM_Encoder_Start>
```

**经验**
`SMS=011` 表示编码器模式 3：`TI1FP1` / `TI2FP2` 都在**双边沿**计数，
所以分辨率是编码器线数的 4 倍（x4 计数）。如果发现读数是实际脉冲数的 4 倍，这是正常的。

---

### 问题 5：编码器启动代码被写进了错误的函数（本次复盘时发现）

**现象**
`Core/Src/tim.c` 里 `HAL_TIM_Encoder_Start()` 出现了两次：
一次在 `MX_TIM4_Init()` 的 `USER CODE BEGIN TIM4_Init 2` 里（**写的是 `htim8`**），
一次在 `MX_TIM8_Init()` 里。同时 TIM4（编码器 2，PB6/PB7）反而没人启动。

**根因**
修改 `tim.c` 时 `USER CODE BEGIN ..._Init 2` 这类锚点文本重复出现，代码被插到了错误的位置。

**影响**
- `MX_TIM4_Init()` 里启动 TIM8：由于 `main()` 中 `MX_TIM8_Init()` 先执行，
  属于"能跑但语义错误"；第二次 `HAL_TIM_Encoder_Start` 会返回 `HAL_BUSY`，无害但冗余。
- 真正的编码器 2（TIM4）永远不会计数。

**解决**
把 `USER CODE BEGIN TIM4_Init 2` 里的代码改成操作 `htim4`：

```c
htim4.Instance->SMCR |= (TIM_SMCR_SMS_1 | TIM_SMCR_SMS_0);
(void)HAL_TIM_Encoder_Start(&htim4, TIM_CHANNEL_ALL);
```

**验证**
`tim.c` 中 `HAL_TIM_Encoder_Start` 只剩两处（第 130 行 `htim4`、第 196 行 `htim8`），
构建退出码 0。

**经验**
`USER CODE BEGIN Xxx_Init 2` 这种锚点在不同函数里是**完全一致**的字符串，
批量编辑时必须带上后续的函数结束标记或更长上下文来消歧，否则很容易插错位置。

---

### 问题 6：TIM2 的 Counter Period 是 4294967295，触发永远不来

**现象**
配好 TIM2 作为 ADC 触发源（TRGO=UPDATE）之后，ADC 一个点都转换不了。

**根因**
`.ioc` 里 `TIM2.IPParameters` 只记录了 `Prescaler` 和 `TIM_MasterOutputTrigger`，
**没有 `Period`**。CubeMX 于是给 32 位定时器 TIM2 填了默认最大值：

```c
htim2.Init.Period = 4294967295;   /* 32 位定时器的默认最大值 */
```

PSC=83 时计数频率 1 MHz，要数 42.9 亿次才产生一次更新事件
→ **71.6 分钟**才来一个 TRGO，看起来就像"完全不工作"。

**解决**
在 CubeMX 里把 TIM2 的 Counter Period 设为 `9`（即 ARR=9，10 个计数一个周期，
对应 100 kSPS），生成后：

```c
htim2.Init.Prescaler = 84-1;
htim2.Init.Period = 10-1;
```

**经验**
**16 位定时器（TIM3/4/5/8…）的 Period 默认是 65535，32 位的 TIM2/TIM5 默认是 4294967295。**
CubeMX 只在你手动改过的参数才写进 `.ioc`，没改过的用外设默认值——
所以"我明明配了推拉/时钟，为什么不动"时，先去看看那个没配的 Period。

---

### 问题 7：配了硬件触发后，软件启动的采样全部失效

**现象**
把 ADC1 的 `ExternalTrigConv` 从 `ADC_SOFTWARE_START` 改成 `ADC_EXTERNALTRIGCONV_T2_TRGO`
之后，原来靠软件启动 + 轮询的单次采样读不出值了。

**根因**
F4 的 ADC `CR2.EXTEN`（触发使能）一旦非 0，硬件就**忽略 `SWSTART`**。
而 `HAL_ADC_Start()` 内部正是在 `CR2.EXTEN == RESET` 的条件下才置 `SWSTART`
（`stm32f4xx_hal_adc.c:806-814`），所以它实际上什么也没做。

**解决**
单次采样时临时把 `EXTEN` 清零，采完再恢复（`User/BSP/Src/ADC_Drv.c`）：

```c
saved_exten = hadc1.Instance->CR2 & ADC_CR2_EXTEN;
hadc1.Instance->CR2 &= ~ADC_CR2_EXTEN;      /* 回到软件启动 */
/* ... HAL_ADC_Start + HAL_ADC_PollForConversion ... */
hadc1.Instance->CR2 |= saved_exten;          /* 原样恢复，不影响帧采集 */
```

同理，`ADC->CCR` 里的多 ADC 模式位也要临时清掉，否则从器件的转换不会同步发生。

**经验**
HAL 的"软件启动"和"硬件触发"是**互斥**的两条路径，切换配置时要显式保存/恢复
`CR2` 和 `CCR` 里的相关位，而不是假设 HAL 会帮你处理。

---

## 三、ADC 与 DMA

### 问题 8：ADC1/ADC2 自检两路读数恒为 0

**现象**
自检程序输出：

```
ADC1(PA0)=   0(   0mV) rng    0-   0 [STUCK-0] | ADC2(PA1)=   0(   0mV) ... | diff 0mV
```

即使把探头接到 3.3 V 也纹丝不动。

**定位过程（关键：不要猜，直接读寄存器）**
在自检里加一段一次性诊断打印，把关键状态寄存器全打出来：

```
DIAG CCR=00018006 MULTI=6 DMA=2 ADCPRE=1 | ADC1 CR2=00000401 SR=00000000 DR=4095
                                        | ADC2 CR2=00000400 DR=0 | CDR=00000000
```

一眼就能读出来：

| 寄存器 | 值 | 含义 |
|---|---|---|
| `ADC1->CR2` | `0x401` | bit0 `ADON=1` → ADC1 已使能；bit10 `EOCS=1` |
| `ADC2->CR2` | `0x400` | **bit0 `ADON=0` → ADC2 从未使能** |
| `ADC1->DR` | `4095` | PA0 上确实是 3.3 V，ADC1 本身完全正常 |
| `ADC->CDR` | `0` | 双 ADC 同步模式下 CDR 需要两个 ADC 都转换才更新 |

**根因**
双 ADC 规则同步模式下，**ADC1 是主器件、ADC2 是从器件**。
但在这版 HAL 里：

- `HAL_ADC_Init()`（`stm32f4xx_hal_adc.c:311`）**只配置，不置 `ADON`**；
- 置 `ADON` 的是 `HAL_ADC_Start()`（`:731-743`）；
- 而同步模式下**只能启动主器件**（`HAL_ADCEx_MultiModeStart_DMA()` 也只使能主器件，
  `stm32f4xx_hal_adc_ex.c:613`），`HAL_ADC_Start(&hadc2)` 从来没被调用过。

于是 ADC2 一直停在 `ADON=0`，从器件不转换 → CDR 恒为 0 → 读到的两路都是 0。

**解决**
在 `Core/Src/adc.c` 的 `USER CODE BEGIN ADC2_Init 2` 段内手动使能从器件：

```c
__HAL_ADC_ENABLE(&hadc2);
HAL_Delay(1U);     /* 等 tSTAB（手册约 3us，给 1ms 足够） */
```

**验证**
反汇编 `MX_ADC2_Init` 尾部确认 `ldr → orr.w #1 → str`（写 `CR2.ADON`）+ `bl HAL_Delay`，
且 `.word 0x40012100` 正是 ADC2 基址。

**经验**
**"两个 ADC 读出来一模一样"通常不是信号问题，而是其中一个根本没工作。**
排查第一步不是换接线，而是把 `CR2 / SR / DR` 打出来看 `ADON`、`EOC`、`OVR`。

---

### 问题 9：ADC2 只在复位时测一次，之后一直是同一个值

**现象**
双 ADC 同步采集时，CH1 的读数每帧都在动，CH2 却是一个**零噪声的死数**
（实测恒为 `4071`）：把探头从 VCC 拔下来插到 GND，参数栏各项数值毫无变化。
自检、DMA 测试、正式 App 三条路径表现完全一致。

**错误的第一版结论（留作教训）**
一开始判断成"双 ADC 同步模式下读 `ADC->CDR` 高 16 位不可靠"，于是把自检临时改成
独立模式（退出多 ADC、分别启动分别读 `DR`）绕了过去——**这只是把问题藏起来了**，
正式采集路径一直是坏的。

**真正的根因**
从器件 ADC2 的 `CR2.EXTEN` 仍然是 `00`（软件启动），**它没有自己的触发边沿**。
双 ADC 规则同步模式下 ADC2 只在被使能的那一次转换出一个结果，之后再无触发，
`ADC->CDR` 的高 16 位就一直停在那个旧值上。

**定位方法**（把寄存器直接打到屏上，一行就够）
```
A2c16000401 A2s00000032 A1s00000000 C00018006
```
| 字段 | 判读 |
|---|---|
| `A2c`（ADC2->CR2）bit0 | `1` = ADON 已使能（正常） |
| `A2c` bit[27:24] | `6` = EXTSEL 选的 TIM2_TRGO |
| `A2c` bit[29:28] | `1` = 上升沿。**修之前这里是 `00`，即 `EXTEN=00`** |
| `C`（ADC->CCR） | MULTI=6 / DMA=2 / ADCPRE=1，说明公共配置本来就没错 |
| `A2s`（ADC2->SR）= `0x32` | 停采集**之后**的残影：最后一个点没有 DMA 接收，OVR 置位属正常 |

**解决**（`User/BSP/Src/ADC_Drv.c`）
```c
/* ADC_Drv_Init()：从器件与主器件同源触发，并清掉开机残留的 OVR */
hadc2.Instance->SR = 0U;
{
    uint32_t trig = hadc1.Instance->CR2 & (ADC_CR2_EXTSEL | ADC_CR2_EXTEN);
    hadc2.Instance->CR2 = (hadc2.Instance->CR2 & ~(ADC_CR2_EXTSEL | ADC_CR2_EXTEN)) | trig;
}

/* ADC_Drv_CaptureStart()：每帧开头也清一次 —— OVR 置位会让 ADC 停止转换 */
hadc2.Instance->SR = 0U;
```

**经验**
1. 双 ADC 同步模式下 `HAL_ADCEx_MultiModeStart_DMA()` / `HAL_ADCEx_MultiModeConfigChannel()`
   **都不会给从器件配触发**，CubeMX 也不生成，必须自己把 `EXTSEL/EXTEN` 复制过去。
2. F4 的 ADC 一旦 `OVR` 置位就**停止转换**，所以"只测一次、之后永远不变"优先查 `OVR`。
3. **看到零噪声的固定值，先怀疑数据没更新，而不是信号稳定**——真实模拟量至少有 ±1 LSB 抖动。
4. `ADC->CDR`（`ADC123_COMMON` 基址 `0x40012300`，偏移 `0x08`）低 16 位是 ADC1、高 16 位是 ADC2，
   一次读取拿到两路、没有先后时差；但它只在同步模式且转换真正发生时更新。

---

### 问题 10：DMA 只搬了 1 个点（`moved 1/64 words`）

**现象**
双 ADC 同步 + DMA 采集，期望搬满 64/1024 个点，实际只有第一个字有效，
其余全是哨兵值 `FFFFFFFF`。

**根因（两个叠加，前者掩盖了后者）**

**① `CCR.DDS` 被 HAL 覆盖**
`HAL_ADCEx_MultiModeStart_DMA()` 内部会按 `hadc1.Init.DMAContinuousRequests`
**重写** `ADC_CCR` 的 `DDS` 位（`stm32f4xx_hal_adc_ex.c:685-694`，DISABLE 时清零）。
在外面直接 `ADC->CCR |= ADC_CCR_DDS` 会在启动瞬间被抹掉。
反汇编铁证：

```
8001d10: ldrb.w r3, [r3, #48]   ; 读 hadc1+0x30 = Init.DMAContinuousRequests
8001d1c: orr.w  r2, r3, #0x2000 ; 置 DDS (bit13)
8001d2a: bic.w  r2, r3, #0x2000 ; 或清 DDS
```

**② `HAL_ADCEx_MultiModeStart_DMA()` 根本不写 `CR2.DMA`**

对比单路模式：`HAL_ADC_Start_DMA()` 在 `stm32f4xx_hal_adc.c:1460` 有

```c
hadc->Instance->CR2 |= ADC_CR2_DMA;
```

而 `HAL_ADCEx_MultiModeStart_DMA()`（`stm32f4xx_hal_adc_ex.c:598-717`）**整段没有这一句**。
多 ADC 模式下 DMA 请求是由主器件的 `CR2.DMA` 这一位开门的，
不置它就只有第一个请求能过去 —— 这正好解释了"恰好 1 个字"。

**解决**
在驱动里自己置位（`User/BSP/Src/ADC_Drv.c`）：

```c
hadc1.Instance->CR2 |= (ADC_CR2_CONT | ADC_CR2_DMA);   /* 连续转换 + DMA 请求使能 */
if (HAL_ADCEx_MultiModeStart_DMA(&hadc1, buf, len) != HAL_OK) { ... }

/* 结束 */
hadc1.Instance->CR2 &= ~(ADC_CR2_CONT | ADC_CR2_DMA);
```

同时把 `hadc1.Init.DMAContinuousRequests` 临时设为 `ENABLE`（用完恢复），
否则 `CCR.DDS` 仍会被 HAL 清掉。

**验证**
`moved 64/64 words`、`搬满 1024/1024 点`；调试快照：

```
CR2=00000503  → ADON(bit0) + CONT(bit1) + DMA(bit8) + EOCS(bit10)
CCR=0001A006  → MULTI=6 + DMA=2 + ADCPRE=1 + DDS(bit13)
DMA_CR=0000551F → 最低位 EN=1
```

**经验**
**HAL 不是万能的，多器件模式（MultiMode）的实现经常比单路模式"少一句"。**
当出现"恰好第一个/最后一个数据有效"的现象时，优先怀疑：
① 外设的 DMA 请求使能位没置；② DMA 流没使能；③ 传输长度被截断。

---

## 四、应用逻辑

### 问题 11：按下 Key3/Key4，LED 只亮灭、不闪烁

**现象**
期望按住按键时 LED 闪烁，实际只是翻转一次亮灭状态。

**根因（代码里根本没有闪烁逻辑，三层叠加）**

1. `User/BSP/Src/LED.c` 的 `LED_Turn()` 只是**翻转一次**引脚电平。
2. `User/BSP/Src/Key.c` 的 `Key_Tick()` 状态机只在**松手沿**上报一次事件：
   ```c
   if (CurrState == 0 && PrevState != 0) { Key_Num = PrevState; }
   ```
   而且每调用 20 次才采样一次。按住不放只会产生 1 个事件；按得太短（<20 次调用）
   甚至一个事件都收不到。
3. 主循环里是 `LED_Turn(); HAL_Delay(500);` —— 延迟在翻转**之后**，没有第二次翻转。
   而且 `HAL_Delay` 是阻塞的，阻塞期间按键扫描也停了。

**为什么会这样（设计层面）**
"闪烁"需要的是**周期性状态机**，而现有代码是**边沿触发的一次性动作**。
这两者不是加个 `HAL_Delay` 就能互相转换的：阻塞延时会把整个主循环卡住，
和"要同时扫描按键"直接冲突。

**正确的做法（方案，未实施）**
用非阻塞的软件定时器模式，在 1 ms 或 10 ms 的节拍里推进：

```c
static uint32_t s_next_tick;
static uint8_t  s_blink_on;
static uint32_t s_period_ms;

void LED_BlinkEnable(uint32_t period_ms);   /* period_ms = 0 表示停止闪烁 */
void LED_Tick(void)                          /* 每 1ms 调用一次 */
{
    if (s_period_ms == 0U) return;
    if ((HAL_GetTick() - s_next_tick) < (s_period_ms / 2U)) return;
    s_next_tick = HAL_GetTick();
    s_blink_on  = !s_blink_on;
    /* 按 s_blink_on 设置 PC8 电平 */
}
```

这样主循环永远不会被阻塞，按键扫描与闪烁互不干扰；
如果之后启用 FreeRTOS，把 `LED_Tick` 放进 1 ms 周期任务即可，逻辑不用改。

**经验**
**"定时/闪烁/超时"这类需求，一律不要用阻塞延时实现。**
判断标准很简单：如果加上延时之后，别的功能"响应变慢"或"漏事件"，就说明方向错了。

---

## 五、硬件与环境

### 问题 12：串口助手收不到任何数据

**根因**
板载的 CH340 USB 转串口**并没有连接到 MCU**（原理图上 TXD/RXD 是断开的），
USART1 的 PA9(TX)/PA10(RX) 悬空，自然收不到。

**解决**
用杜邦线手工跳线：

```
CH340 的 TXD  →  PA10 (USART1_RX)
CH340 的 RXD  →  PA9  (USART1_TX)
GND 共地
```

**经验**
"代码没问题但串口没输出"时，先分清是**软件没发**还是**硬件没连**：
把 TX 脚用示波器/逻辑分析仪看一眼，或者直接把 TX 短接到 RX 做回环测试，
能立刻把问题范围砍一半。

---

## 六、LCD 驱动的实现方法（10 月 20 日）

这一节不写"踩了哪个坑"，而是把驱动从零做起来的**设计思路**记下来，
方便以后换屏、加页面时复用同一套结构。

### 6.1 硬件与总线

| 项目 | 值 |
|---|---|
| 屏 | ST7789，2.4 寸 IPS，原生 240×320，**无 MISO**（只写不读） |
| 总线 | SPI1，单向 2Lines，8 bit，MSB first |
| 模式 | `CPOL=HIGH` + `CPHA=2EDGE` → **SPI 模式 3** |
| 速率 | `SPI_BAUDRATEPRESCALER_8` → 84 MHz / 8 = **10.5 Mbit/s ≈ 1.31 MB/s** |
| 引脚 | SCK=PA5、MOSI=PA7（AF5）；CS=PA4、DC=PB0、RES=PB1、BLK=PB2（推挽输出，初值高） |

**关键结论：SPI 才是瓶颈。** 全屏 320×240×2 = 153 600 B，按 1.31 MB/s 算
≈ **117 ms/帧**，物理上限只有 8.5 fps。所以后面所有显示优化，本质都是"少发字节"。

### 6.2 初始化序列

顺序是有讲究的，冷启动（硬复位）和软复位都要覆盖：

```
硬复位 RES 拉低 10 ms → 拉高 → 等 120 ms
SWRESET(0x01) + 120 ms          ← 软复位也要等
SLPOUT(0x11)  + 120 ms          ← 退出睡眠
COLMOD(0x3A) = 0x55             ← RGB565，16 bit/像素
MADCTL(0x36) = 0x60             ← 横屏 320×240（方向不对只改这一个宏）
PORCTRL/GCTRL/VCOMS/LCMCTRL/VDVVRHEN/VRHS/VDVS/FRCTRL2/PWCTRL1
PVGAMCTRL(0xE0) 14 字节 / NVGAMCTRL(0xE1) 14 字节
INVON(0x21)                     ← IPS 面板通常需要反显
NORON(0x13)
Lcd_Fill(BLACK)                 ← 先清屏，避免上电花屏
DISPON(0x29) + 100 ms
开背光 BLK = 1
```

**经验**：`MADCTL` 一个字节决定旋转方向，`0x60` 是横屏；方向不对时优先改它，
不要去改所有绘制函数的坐标。若颜色反了（红蓝互换），把 `0x60` 改成 `0x68`（BGR 位）。

### 6.3 写屏的两条铁律

**① 一次窗口、批量写 —— 绝不每点设窗口**

```
Lcd_SetWindow(x, y, w, h);      ← 只写一次 CASET/RASET
Lcd_PushColors(buf, count);     ← 连续推 count 个像素
```

每设一次窗口都要发 `CASET`+`RASET`+`RAMWR` 三条命令（约 10 字节命令开销），
逐像素设窗口的话，1200 个像素的命令开销就超过数据本身。

**② RGB565 必须转大端**

STM32 是小端，屏要的是高字节先出。驱动里用一块 `static uint8_t s_tx[128*2]`
做 128 像素一批的字节交换，避免为整行再开一块 640 B 的临时缓冲。

### 6.4 字模：为什么最后是手写点阵

依次试过三种方案，全部失败：

| 方案 | 结果 |
|---|---|
| `consola.ttf` 11/12 px + 阈值 96/160/192/224 | 糊成一团，`0`/`8` 变实心块；阈值调高又断笔 |
| Pillow `load_default()` | 是 FreeType 矢量（Aileron 10px），阈值 128 时 `5` 的左竖丢失 |
| Pillow `load_default_imagefont()` | 确是点阵，但笔画 2px，6×10 格子里 `A` 渲染残缺 |

**根因**：TrueType 在 6×10 这种尺寸下抗锯齿会串色，任何阈值都无法同时救"粗"和"断"。

**最终方案**：手写 5×7 点阵，字形用 `#`/`.` 字符串写在 `tools/gen_font.py` 的
`GLYPHS` 字典里，脚本生成 `User/BSP/Src/Font.c`（`95 × 7 = 665 B`）。
- 单元 6×9（右 1 px、下 2 px 间隔），小写字母映射成大写
- 大字号不另存表，用 `Font_Expand2()` 把每行掩码 2 倍展开 → 12×18，表大小仍为 0
- `python tools/gen_font.py --preview` 可以直接在终端预览字形，改字模不用烧板子

### 6.5 分层：谁允许碰 HAL

```
App      App_Scope.c  ScopeMenu.c        ← 按键、状态机、编排
View     View.c  WaveView.c  StatusBar.c  ParamBar.c   ← 布局、配色、增量逻辑
BSP      Lcd.c  Font.c  Frontend.c  ADC_Drv.c ...      ← 唯一接触 HAL 的一层
```

`View` 里**不允许** `#include "spi.h"` / `"adc.h"`，它只知道"给我一组 mV 和 V/div，
我把它们画成像素"。换屏时只改 `Lcd.c`；换采样器件时只改 `ADC_Drv.c`。

### 6.6 布局与坐标映射

```
(0,0)                                               (319,0)
┌───────────────────────────────────────────────┐
│ 状态栏 22 px                                  │
├───────────────────────────────────────────────┤
│ 波形区 190 px：8 列 × 10 行，每格 40 × 19 px   │  ← 中轴线 y = 117
├───────────────────────────────────────────────┤
│ 参数栏 28 px                                  │
└───────────────────────────────────────────────┘
```

垂直映射（**标定已在 App 层由 `Frontend` 完成，View 只做线性映射**）：

```c
y = VIEW_WAVE_MID_Y - (mv * VIEW_PX_PER_VDIV) / vdiv_mv;   /* 117 - mv*19/vdiv */
y = clamp(y, VIEW_WAVE_Y, VIEW_WAVE_Y + VIEW_WAVE_H - 1);  /* 22 ~ 211 */
```

0 V 固定画在中轴线上，负电压自然落在下方——这也决定了改造后的前端
（带 1.65 V 偏置）必须先把偏置扣掉，否则 0 V 会跑到中心以上。

### 6.7 增量刷新（把 8 fps 拉到 30+ fps 的核心）

**先算账**（旧实现，每帧）：

| 项目 | 字节 |
|---|---|
| 波形区整片填充 320×190 | 121 600 |
| 网格线 | ≈ 10 000 |
| 曲线 | ≈ 2 000 |
| 状态栏整条 | 14 080 |
| 参数栏整条 | 17 920 |
| **合计** | **≈ 166 KB → 127 ms → 8 fps** |

**优化思路：把"每帧重画"改成"只重画变了的行"**，共四层：

1. **脏行位图**：190 行用一个 24 B 的位图标记"这行要发"。
2. **行模板**：背景色是规律的（边框/网格线/普通行只有 3 种），预先算好
   `s_row_normal[320]`（640 B），普通行直接 `memcpy` 出来，不用逐像素判断。
3. **行指纹（关键）**：每行合成完算一个 32 位 FNV-1a，与上一帧同行的哈希比较，
   **相同就整行不发 SPI**。波形静止或噪声很小时，绝大多数行都被跳过。
4. **上下条带各自增量**：
   - 状态栏：把整个 `StatusBar_Info` 与上一帧 `memcmp`，没变化直接跳过（省 14 KB/帧）
   - 参数栏：五个字段各自 `strncmp`，只重画变了的字段，并用背景色清掉旧值尾巴

**结果显示区域典型帧只发 30~50 KB → 23~38 ms → 26~43 fps**，
额外 RAM 只花了约 4.75 KB（行模板 640 B + 行缓冲 640 B + 前后 y 数组 + 指纹 760 B + 位图 24 B），
没有采用"121.6 KB 影子缓冲"的方案。

**增量刷新的三个坑（都踩过）**：

- **通道隐藏时要擦旧轨迹**：CH2 关掉后不能只"不画新的"，还要把上一帧的位置用背景刷掉，
  否则会留一条残影。做法是隐藏时对 `s_prev_y` 走一遍 `Wave_MarkPrev()`。
- **弹窗会盖住波形区**：菜单退出后必须整片重画（`WaveView_Invalidate()` 把 `s_valid` 清 0，
  下一帧 `Wave_DirtyAll()` 重建全部 190 行），否则弹窗底下那一块永远是旧的。
- **提示文字会被自动擦掉**：上电的 `PRESS KEY4 TO RUN` 是画在波形区里的，
  `WaveView_DrawMessage()` 结束时把 `s_valid` 清 0，第一次真正画波形时整片重建就会覆盖它，
  不需要额外的擦除逻辑。

### 6.8 颜色：配置值不等于显示色

`CH1 = 0xFFE0`（RGB565 里是**黄**）实测显示**蓝**，`CH2 = 0x07FF`（**青**）实测显示**红**。
这是面板的 RGB/BGR 顺序与配置不一致（`MADCTL` 的 BGR 位），不是代码 bug。
处理办法：**颜色宏不改**，只把注释和文档改成"配置为黄，实际偏蓝"，认线按实际显示来；
若确实想纠正，把 `LCD_MADCTL` 从 `0x60` 改成 `0x68`。

---

## 七、10 月 20 日新增问题

### 问题 13：编码器转一格，数值一下跳 4

**现象**
`Encoder_Get()` 每转一格（一个定位点）返回值变化 4，调 V/div 时一档一档蹦。

**根因**
TIM8/TIM4 配的是 `Encoder Mode TI1 and TI2`，即 **x4 计数**：
A/B 两相的上升沿、下降沿都计数，所以一个机械定位点 = 4 个计数。

**解决**
不改 CubeMX 配置，在驱动层折算（`User/BSP/Src/EncoderDrv.c`）：

```c
#define ENCODER_DRV_COUNTS_PER_DETENT  4

/* 必须做余数累加，不能直接 raw_delta / 4 */
static int16_t EncoderDrv_ToDetents(int16_t raw_delta, int32_t *rem)
{
    int32_t total = *rem + (int32_t)raw_delta;
    int16_t detents = (int16_t)(total / ENCODER_DRV_COUNTS_PER_DETENT);

    *rem = total - (int32_t)detents * ENCODER_DRV_COUNTS_PER_DETENT;
    return detents;
}
```

**为什么不能直接除**
主循环一帧可能超过 100 ms，期间编码器可能只转了"1 个计数"。
直接 `1/4 = 0`，四次就被整除丢掉——**慢速旋转会丢步**。
余数累加把"没凑够一格"的零头留到下一次，一格都不丢。

**经验**
编码器"一转就跳 4"是 x4 计数的正常行为，不要改硬件滤波或降计数模式，
**在软件里折算成"格"更稳**；而折算必须带余数。

---

### 问题 14：双通道波形完全重合，而且 `RUN`/`HOLD` 像是反的

**现象**
CH1 接 GND、CH2 接 VCC，屏幕上两条线却完全重叠；按 KEY4 切 RUN/HOLD 时，
画面看不出任何变化，像是显示反了。

**定位过程**
先排除显示层：`y = 117 - (v_mv - 0)*19/500`，0 V → y≈179、3.3 V → y≈55，
**数据正确时两条线数学上不可能重合** → 说明两路数据相同，或者数据根本没更新。

加诊断行打印两路码值与采集状态码后，读到：

```
1:0-0  2:0-0  w0FD7000x  st3
```

`st3` = `TIMEOUT`，而 `w0` 每帧都在变（说明 DMA 确实搬进了至少一个字）。
于是把怀疑转向"完成标志没置位"。

**根因（决定性，A/B 两层）**

**A. `taskENTER_CRITICAL()` 在调度器没启动时把中断永久关死**

`ADC_Drv_CaptureStart()` 原来用任务级临界区保护启动时序：

```c
taskENTER_CRITICAL();   /* 改 s_busy / s_done / s_error */
...
taskEXIT_CRITICAL();
```

但本工程 `Core/Src/main.c` 里是 `// freertos_app_start();`（**调度器没启动**），而：

| 事实 | 位置 |
|---|---|
| `uxCriticalNesting` 的**初值是 `0xaaaaaaaa`**（不是 0） | `ThirdParty/FreeRTOS/Source/portable/GCC/ARM_CM4F/port.c:161` |
| 只有 `xPortStartScheduler()` 会把它清 0 | 同文件 `:443` |
| `vPortExitCritical()` 只在计数归 0 时才 `portENABLE_INTERRUPTS()` | 同文件 `:493-502` |

于是 `enter` → `0xAAAAAAAB`，`exit` → `0xAAAAAAAA` ≠ 0 → **`PRIMASK` 永远是 1**，
之后所有中断都不再响应：TIM7 时基（按键扫描就在里面）、DMA2_Stream0 完成中断全部停摆。

**B. 由此产生的连锁**

`s_done` 只能由 DMA 完成中断置位 → 采集永远超时 → `App_Capture()` 返回失败 →
`s_ch1[]/s_ch2[]` 保持上电时的全 0 → 两条线都画在同一条 y 上**完全重合**；
而画面因为数据不变、增量刷新判定"无变化"，一帧都不发 → 看起来像 HOLD。
`StatusBar.c` 里 `(running != 0U) ? "RUN" : "HOLD"` 本身是对的，只是画面不动了。

**解决**
临界区按调度器状态分支（`User/BSP/Src/ADC_Drv.c`）：

```c
if (adc_drv_sched_running() == pdTRUE)
{
    taskENTER_CRITICAL();
    ... 改 s_busy / s_done / s_error ...
    taskEXIT_CRITICAL();
}
else
{
    uint32_t primask = __get_PRIMASK();   /* 裸机：自己保存/恢复 PRIMASK */
    __disable_irq();
    ... 改 s_busy / s_done / s_error ...
    __set_PRIMASK(primask);
}
```

**验证**
修复后诊断行变为 `1:0-x 2:4039-4039 N0 w0FC7000y ST0`（`N0` = NDTR 归零，320 点全搬满），
两路线终于分离。

**经验**
**"用了 FreeRTOS 的 API" ≠ "FreeRTOS 在运行"。**
只要调用了 `taskENTER_CRITICAL()` / `vTaskDelay()` 这类 API，就必须先确认
`xTaskGetSchedulerState()`，否则在裸机下会出现"静默关中断""永久阻塞"这类
极难排查的故障。**这类 bug 的表象总是"数据不动"，而不是"报错"。**

---

### 问题 15：修好 CH2 之后，自检程序报 `READ FAIL`

**现象**
为了让 ADC2 也能被 TIM2 触发，在 `ADC_Drv_Init()` 里把 ADC1 的
`(ADC_CR2_EXTSEL | ADC_CR2_EXTEN)` 复制给了 ADC2。CH2 恢复正常，
但轮询式自检 `ADC_Drv_ReadOnce()` 开始超时。

**根因**
`HAL_ADC_Start()` 内部只在 `CR2.EXTEN == 0`（软件触发）时才置 `SWSTART`
（`stm32f4xx_hal_adc.c:797`）。`ReadOnce()` 只清了 `hadc1` 的 `EXTEN`，
**ADC2 的 `EXTEN` 还留着**，于是 `HAL_ADC_Start(&hadc2)` 永远不启动 → 轮询 20 ms 超时。

**解决**
`ADC_Drv_ReadOnce()` 对两个 ADC 都做保存/清除/恢复：

```c
saved_exten1 = hadc1.Instance->CR2 & ADC_CR2_EXTEN;
saved_exten2 = hadc2.Instance->CR2 & ADC_CR2_EXTEN;
hadc1.Instance->CR2 &= ~(ADC_CR2_EXTEN | ADC_CR2_DMA);
hadc2.Instance->CR2 &= ~(ADC_CR2_EXTEN | ADC_CR2_DMA);
... 采样 ...
hadc1.Instance->CR2 |= (saved_exten1 | saved_dma1);
hadc2.Instance->CR2 |= (saved_exten2 | saved_dma2);
```

**经验**
**"为了修 A 而改的全局配置，会打破 B 的隐含前提。"**
多路径共用同一个外设时，每条路径都必须自己把状态恢复干净，
并且在改动后把**所有**调用路径都回归一遍（这里就是自检路径和采集路径）。
`CR2.DMA` 也要一起清——`HAL_ADC_PollForConversion()` 在 `EOCS && DMA`
同时置位时会立刻返回 `HAL_ERROR`。

---

### 问题 16：波形在动，`RMS` 和 `DC` 却一直不动

**现象**
CH1 波形随探头变化，但参数栏的 `RMS`、`DC` 始终是上电时的值。

**根因（两层）**
1. 主循环把测量关在"采集成功"分支里：

   ```c
   if (App_Capture() != 0U) { Measure_Analyze(src, ...); }
   ```

   而波形是无条件用 `s_ch1` 画的（`App_Capture()` 无论成败都拆缓冲）。
2. `App_Capture()` 几乎每帧都返回失败：帧尾最后一两个转换没有 DMA 接收，
   会置 `OVR` → `HAL_ADC_ErrorCallback()` 置 `s_error=1` →
   `ADC_Drv_CaptureWait()` 结尾把**整帧**判成 `ERROR`，尽管 320 点早已搬完。

**解决**

驱动侧改成"数据完整性优先"（`ADC_Drv_CaptureWait()` 结尾）：

```c
ADC_Drv_CaptureStop();

/* 整帧搬满（NDTR 归零）就算成功，帧尾的 OVR 只是残影 */
if ((s_done != 0U) && (s_last_ndtr == 0U))
{
    ret = ADC_DRV_OK;
}
else if ((ret == ADC_DRV_OK) && (s_error != 0U))
{
    ret = ADC_DRV_ERROR;
}
```

App 侧让测量跟着缓冲走：

```c
(void)App_Capture();                       /* 成功与否都往下走 */
Measure_Analyze(src, APP_CAP_LEN, s_st.sps, &s_meas);
```

**经验**
**"波形活、数字死"几乎一定是两条路径读了不同的数据源。**
让"显示"和"测量"共用同一份缓冲、同一个判定，就不会再出现这种撕裂。

---

### 问题 17：状态栏本该显示 `DCx1`，实际显示 `Dx1`

**根因**
格式化时把字符串指针当成了字符：

```c
snprintf(buf, sizeof(buf), "%cx%u", 'D' | 'A', atten);   /* 错 */
```

`%c` 只取一个 `char`，`"DC"` 这个指针被截断成首字符 `'D'`。

**解决**
```c
snprintf(buf, sizeof(buf), "%s %c", (ac_dc != 0U) ? "AC" : "DC",
         (range != 0U) ? 'S' : 'W');      /* DC S / DC W */
```

**经验**
`%c` 吃的是**整数**，传字符串字面量在某些编译器上只是 warning（指针截断），
运行期不会崩，只会打出莫名其妙的字符。见到"少字母"的显示问题先查格式符。

---

### 问题 18：编码器1 只改一个通道的 V/div

**现象**
转编码器1 时只有一条线缩放（实际显示为蓝色的那条），另一条不动。

**根因**
`App_HandleMain()` 里 `ch = measure_ch - 1;` 只改**当前测量通道**的
`vdiv_idx[ch]`。这不是逻辑错误，而是"分通道 V/div"的设计取舍——
只是用户期望的是"同屏两路用同一套分辨率"。

**解决**
`App_State.vdiv_idx` 从 `uint8_t[2]` 改成**单个 `uint8_t`**，双通道共用；
主界面编码器1、菜单、状态栏、波形映射全部取同一个值。
分通道微调的能力放到以后再考虑。

**经验**
**"共用档位"比"每通道一份"更容易解释也更容易对齐**：
同屏两条线用同一套电压-像素换算，才不会出现"看起来一样的幅度其实读数不同"。

---

### 问题 19：按 KEY2 退出菜单，弹窗又冒出来

**现象**
按 KEY2 后 `SETTINGS` 弹窗内容消失了一瞬间，随后又画了回来，
看起来"只是把窗口擦了，其实还留在菜单里"。

**根因**
`App_Scope_Loop()` 的退出路径把 `s_st.menu` 置 0 之后，**仍然无条件调用 `App_DrawMenu()`**：

```c
redraw = App_HandleMenu(d1, d2, key);
if (redraw) { App_DrawWave(); App_DrawBars(); App_DrawMenu(); }   /* ← 错 */
return;
```

置 0 只改了状态，屏幕上的弹窗还在；重画一次就"复活"了。

**解决**
弹窗绘制只在"确实还在菜单里"时执行，退出时贯穿到主界面重画流程：

```c
if (s_st.menu != 0U)
{
    if (redraw) { App_DrawWave(); App_DrawBars(); App_DrawMenu(); }
    return;
}
redraw = 1U;    /* 刚退出：本帧按主界面完整重画，且不画弹窗 */
```

**经验**
**状态机里"改状态"和"改画面"必须成对**。
凡是 `if (state)` 里画某物的，退出那次转换就不能再走那条分支。

---

### 问题 20：按 KEY2 进菜单不显示，转一下编码器才出来

**现象**
修完问题 19 后出现新问题：按 KEY2 没有任何反应，转动编码器后菜单才出现。

**根因**
这个问题 19 的修法带来的**副作用**：进入菜单的那一帧走的是主流程收尾
`if (redraw) { App_DrawWave(); App_DrawBars(); }`，里面**没有** `App_DrawMenu()`。
下一帧进菜单分支时若没有任何输入，`App_HandleMenu()` 返回 `redraw == 0` 直接 `return`，
弹窗就一直不出现——直到转编码器产生 `redraw == 1` 才第一次画。

**解决**
在收尾重画里补一行，前提仍然是 `s_st.menu != 0U`：

```c
if (redraw != 0U)
{
    App_DrawWave();
    App_DrawBars();

    if (s_st.menu != 0U) { App_DrawMenu(); }   /* 进入菜单那一帧 */
}
```

**验证**（四条状态转换逐一核对）
进入菜单 → 当帧出弹窗；菜单中无输入 → 不重画；菜单中转编码器 → 重画数值；
菜单中按 KEY2 → 当帧退出并完整重画主界面。

**经验**
修一个状态机 bug 时，**要把"进入 / 停留 / 操作 / 退出"四种转换全部列出来过一遍**。
只盯着出错的那一条，很容易把问题挤到相邻的分支上。

---

### 问题 21：面板显示的颜色和配置值相反

**现象**
代码里 CH1 配 `0xFFE0`（黄）、CH2 配 `0x07FF`（青），实际显示是**蓝**和**红**。

**根因**
面板的 RGB/BGR 数据顺序与 `MADCTL` 配置不一致（`0x60` 与 `0x68` 的差别就是 BGR 位）。

**解决**
不改颜色宏，只把注释和文档改成实机表现（`ScopeView.h`、`LCD_UI_DESIGN.md`、`CONTROLS.md`）；
真要纠正就把 `LCD_MADCTL` 改成 `0x68`。

**经验**
**RGB565 的数值不能字面理解"红色 = 0xF800 就一定红"**，
最终颜色由面板的子像素顺序决定。显示类问题先分清"配置错"和"面板反了"。

---

### 问题 22：帧率只有约 8 fps

详见 **6.7 增量刷新**。一句话：每帧整片重画约 166 KB，SPI 只有 1.31 MB/s，
127 ms/帧就是物理上限。

**解决**：脏行位图 + 行模板 + **行指纹（32 位 FNV-1a）** 跳过未变化的行 + 上下条带各自增量。
典型帧从 166 KB 降到 30~50 KB，帧率提升到 26~43 fps。

**经验**
**先算带宽，再写代码。** 知道"1.31 MB/s"和"每帧 166 KB"之后，
优化方向（少发字节）是自然算出来的，而不是靠猜。

---

### 问题 23：前端零点/增益算错，直到被实测数据揭穿

**现象**
按纸面推导写的参数表（零点 157 mV / 1171 mV、增益 1/10.5 与 1/1.41、
"负向恒为 −1.65 V"）与实际读数对不上：实测大量程 1941、小量程 1320（输入接 GND）。

**根因**
把偏置的**戴维南电阻 `R_th`（500 Ω = 1 kΩ ∥ 1 kΩ）**当成了下拉电阻 `R_down`（1000 Ω）。
实际上 `Rs`（10 kΩ 或 909 Ω）与下拉电阻是**并联**关系，不是串联。

正确的节点方程（基尔霍夫）：

```
(V − 3.3)/1k + V/1k + (V − Vin)/Rs = 0
→ V = V_th + (Vin − V_th)·g,   V_th = 1.65 V,  R_th = 500 Ω,  g = R_th/(R_th + Rs)
```

推论：

| 量 | 表达式 |
|---|---|
| 零点引脚电压 | `V_th·(1 − g)` |
| 可测下限 | `−V_th·(1/g − 1)` |
| 可测上限 | `+V_th·(1/g + 1)` |
| 窗口宽度 | `3.3 V / g`（**中心恒在 +1.65 V**） |

修正后的数值：大量程 `g = 1/21`、零点 1.571 V（raw 1950）、−33.0…+36.3 V；
小量程 `g = 1/2.818`、零点 1.065 V（raw 1321）、−3.00…+6.30 V。
用实测反算：1941 vs 1950（−0.5 %）、1320 vs 1321（−0.1 %）→ **硬件是对的，参数表是错的**。

**解决**
1. 把推导写进 `tools/calc_frontend.py`（KVL 模型 + 自检码值表 + 实测校验行），
   所有数字由脚本生成，不再手算。
2. `User/BSP/Src/Frontend.c` 里只保留**一张两行的标定表**（零点码值、零点 mV、增益分子/分母），
   换算用 int64 防溢出。
3. 文档 `FRONTEND_BIAS_DESIGN.md` 第七/八/十一节按新模型重写，并在开头留修订记录。

**经验**
**纸面推导必须被实测校准一次。**
这次是"两个实测点"直接把错误模型否掉的——如果只测一个点，很可能会用"标定系数"
把错误掩盖过去，然后在别的量程上翻车。**用 GND 和 3.3 V 两个已知点反算，
是最便宜的正确性检查。**

---

### 问题 24：CH2 还没搭电路时读数 3.27 V，看起来像短路

**现象**
CH2 尚未焊接，读数稳定在 4060~4073（≈3.27 V）、spread 20~50。

**根因**
PA1 高阻悬空，节点上唯一连接是"防过压肖特基（ADC → 3.3 V）"，
其**反向漏电流**足以把高阻节点拉到接近 VCC。
spread 20~50 恰恰证明是高阻（硬短路只有 1~2 的跳动）。

**解决**
自检程序对"未搭建通道"单独给提示（后来 CH2 搭好后这段特判已删除）；
`readonly` 的判据是**噪声幅度**：spread 大 = 高阻，spread 极小 = 短路或固定电平。

**经验**
**悬空引脚读数"贴着电源轨"是正常现象**，不要急着怀疑短路；
先看噪声幅度，再看是否随手指/探头移动。

---

### 附：10 月 20 日的几处行为变更（是需求，不是 bug）

这些不是故障，但改变了界面行为，复盘时一并记下，免得以后误以为是回归：

| 变更 | 原因 |
|---|---|
| 上电停在 `HOLD`，显示 `PRESS KEY4 TO RUN`，按 KEY4 才开始采集 | 避免一上电就用没接线/悬空的输入画出假波形 |
| `HOLD` 时**保留最后一帧的测量值** | 大多数示波器都这样，冻结画面方便读数；且数据不变时增量刷新一字节都不发 |
| 双通道共用同一个 V/div（`s_st.vdiv_idx` 从数组改成单值） | 同屏两路用同一套电压-像素换算，避免"看起来一样却读数不同" |
| V/div 扩到 7 档，并与前端量程联动（`S`/`W`） | 加 10 V/div 才能把大量程 ±33 V 的窗口一屏显示完 |
| KEY1 只切换"测量通道"，不影响显示通道（KEY3 管显示） | 参数栏只需要一个通道的数值 |

---

## 八、关键 HAL 源码事实（这次调试的真正收获）

以下每一条都是**读 HAL 源码确认**的，不是猜的。它们解释了大量"配置看起来对但不动"的现象：

| # | 事实 | 位置 |
|---|---|---|
| 1 | `HAL_ADC_Init()` 只配置，**不置 `ADON`**；置 `ADON` 的是 `HAL_ADC_Start()` | `stm32f4xx_hal_adc.c:311` / `:731-743` |
| 2 | **`HAL_ADCEx_MultiModeStart_DMA()` 不置 `CR2.DMA`**，单路的 `HAL_ADC_Start_DMA()` 才置 | `stm32f4xx_hal_adc_ex.c:598-717` / `stm32f4xx_hal_adc.c:1460` |
| 3 | `HAL_ADCEx_MultiModeStart_DMA()` 会用 `hadc.Init.DMAContinuousRequests` **重写 `CCR.DDS`** | `stm32f4xx_hal_adc_ex.c:685-694` |
| 4 | `HAL_ADCEx_MultiModeStart_DMA()` 只使能**主器件**，不碰从器件 | `stm32f4xx_hal_adc_ex.c:613` |
| 5 | **`HAL_TIM_Encoder_Start()` 不设置 `SMCR.SMS`**，只做通道使能 + `CEN=1` | `stm32f4xx_hal_tim.c:3214` |
| 6 | `HAL_ADC_PollForConversion()` 在 `EOCS && DMA` **同时**置位时立即返回 `HAL_ERROR` | `stm32f4xx_hal_adc.c:892-902` |
| 7 | `ADC_EOC_SINGLE_CONV = 0x1` 实际把 `CR2.EOCS`（bit10）**置 1**，容易误判为"多出来的位" | `stm32f4xx_hal_adc.h:407-408` |
| 8 | `ADC->CDR` 低 16 位 = ADC1，高 16 位 = ADC2 | RM0090 |
| 9 | 寄存器位：`CR2.ADON=bit0`、`CR2.DMA=bit8`、`CR2.DDS=bit9`、`CR2.EOCS=bit10`、`CCR.DDS=bit13` | `stm32f407xx.h` |
| 10 | **`uxCriticalNesting` 初值是 `0xaaaaaaaa`**，只有 `xPortStartScheduler()` 清 0；`vPortExitCritical()` 仅在计数为 0 时开中断 | `portable/GCC/ARM_CM4F/port.c:161` / `:443` / `:493-502` |
| 11 | `HAL_ADC_Start()` 仅在 `CR2.EXTEN == 0` 时才置 `SWSTART` | `stm32f4xx_hal_adc.c:797` |
| 12 | `HAL_ADCEx_MultiModeConfigChannel()` 只写 `CCR` 的 MULTI/DMA/DELAY，**不碰 `CR2`** | `stm32f4xx_hal_adc_ex.c:999-1009` |
| 13 | `DMA2_Stream0` 由 `HAL_DMA_IRQHandler(&hdma_adc1)` 在中断里推动；普通模式下 `NDTR` 归零即可判定"搬完" | `Core/Src/stm32f4xx_it.c:220-225` |
| 14 | ST7789 `MADCTL` 的 BGR 位（`0x60`↔`0x68`）决定红蓝是否互换 | ST7789 数据手册 |

**方法论总结**

1. **先读寄存器，再改代码。** 问题 8 的根因（`ADON=0`）是靠打印 `CR2` 一眼看出来的，
   在此之前所有关于"接线/悬空/采样时间"的猜测都是错的。
2. **符号层面的验证要看 map 和 nm**，不能只看"编译通过"。弱符号覆盖尤其如此。
3. **工具函数归工具函数。** `HAL_xxx_Start()` 不一定把所有使能位都置上，
   多器件模式的实现经常"少一句"。
4. **看数据特征反推根因。** "恰好 1 个字"→ 单个 DMA 请求；"恰好 71 分钟"→ 32 位 ARR；
   "两路读数完全一样"→ 其中一路没工作。数据形状本身就带着根因信息。
5. **改完一定量一次。** 采样率不是"设 200k 就是 200k"：实测 199744 / 498507 SPS，
   换算成绝对时间是约 6 µs 的**固定启动开销**（不是速率误差，因为 500k 档的偏差并没有放大 5 倍），
   两档比值 2.4957 ≈ 2.5，说明采样间隔本身是准确、线性不漂移的。
6. **数据形状比报错信息更有用。** "恰好 1 个字"→ 单个 DMA 请求；
   "零噪声的固定值"→ 数据没更新；"两路完全一样"→ 其中一路没工作；
   "看起来像 HOLD"→ 其实是画面根本没刷新。**先假设数据没动，再怀疑算法。**
7. **单位换算只允许存在一处。** ADC 码值 → mV 的零点、增益、量程全部收敛到
   `Frontend.c` 一张标定表里；View 和 Algorithm 只认"mV（带符号）"。
   一旦让"码值"和"mV"在多个模块间混用，标定必然散落成好几份，改一处漏三处。
8. **用已知量校准，而不是相信推导。** 接 GND 得零点、接 3.3 V 得增益，
   两个点就能把整条链路的换算系数钉死；纸面算错时，实测会立刻把它揭穿（问题 23）。

---

## 九、当前代码结构

```
CMakeLists.txt                     ← User/ 自动收集源文件与头文件目录（问题 1）
tools/
  gen_font.py                      ← 生成 5×7 点阵字库；--preview 可预览字形
  calc_frontend.py                 ← 按 KVL 重算前端偏置参数（问题 23）
User/
  BSP/                             ← 驱动层：只有这里碰 HAL
    Inc/  ADC_Drv.h  ADC_SelfCheck.h  Frontend.h
          Lcd.h  Font.h  EncoderDrv.h  Key.h  LED.h  Encoder.h
    Src/  ADC_Drv.c                ← 唯一接触 ADC / TIM2 / DMA2_Stream0 的实现
          Frontend.c               ← 码值 → 输入 mV（零点/增益/量程唯一出处）
          Lcd.c  Font.c            ← ST7789 SPI 屏 + 5×7 点阵字库（665 B）
          EncoderDrv.c             ← 两路编码器增量，四倍频折算成每格 1（问题 13）
          Retarget.c               ← printf → USART1（重写 _write，问题 3）
          ADC_SelfCheck.c          ← 串口自检程序（main.c 开关切换）
          ADC_Test.c  ADC_DMA_Test.c  ← 纯 API 调用示例
          Key.c  LED.c  Encoder.c
  View/                            ← 视图层：只调用 Lcd / Font，不含 HAL
    Inc/  ScopeView.h              ← 布局常量、配色、StatusBar_Info / WaveView_Data
    Src/  View.c  WaveView.c       ← 增量刷新、脏行位图、行指纹（6.7 节）
          StatusBar.c  ParamBar.c  ← 字段级增量
  Algorithm/                       ← 纯计算，不含任何硬件访问
    Inc/  Measure.h
    Src/  Measure.c                ← 峰峰值 / 有效值 / 平均值 / 频率（输入为 mV）
  App/                             ← 应用层：按键、状态机、编排
    Inc/  App_Scope.h  ScopeMenu.h
    Src/  App_Scope.c  ScopeMenu.c
Core/
  Src/adc.c     ← USER CODE BEGIN ADC2_Init 2：__HAL_ADC_ENABLE(&hadc2)（问题 8）
  Src/tim.c     ← USER CODE TIM4_Init 2 / TIM8_Init 2：编码器模式启动（问题 4/5）
  Src/spi.c     ← CubeMX 生成，SPI1 模式 3 / 10.5 Mbit/s
  Src/main.c    ← USER CODE 段：ADC_Drv_Init() → App_Scope_Init()，主循环 App_Scope_Loop()
                  另含 APP_RUN_SELFCHECK 开关（0 = 示波器界面，1 = 串口自检）
```

**分层调用方向（单向，不许反向）**
`App → View → Lcd/Font`、`App → Algorithm`、`App → ADC_Drv`；
View 与 Algorithm 都不包含 `adc.h`/`tim.h`，换屏或换采样器件时它们不用改。

**HAL 回调归属约定**
`HAL_ADC_ConvCpltCallback()` / `HAL_ADC_ErrorCallback()` 由 `ADC_Drv.c` 独占实现，
业务层不要再重复定义（会链接冲突）。

**DMA 缓冲区约束**
采集缓冲区必须放在 SRAM，**不能放 CCMRAM**——DMA1/DMA2 访问不到 CCMRAM
（F4 的 CCMRAM 只挂在 CPU 的 D-bus 上）。分配时用普通全局数组即可。

---

## 十、遗留 / 待办

- [ ] 按键闪烁（问题 11）目前只有方案，未实施。
- [ ] 双通道的 V/div 目前是**共用同一档**（统一分辨率，便于对比两条线）；
      菜单里也只能整体调，不能分通道微调。V/div 同时决定前端量程：
      前 4 档 = 小量程（跳线乙插 `MID`），后 3 档 = 大量程（跳线乙不插），
      软件检测不到跳线，只能靠状态栏的 `S`/`W` 提醒用户对齐。
- [ ] **前端标定还只是"设计值 + 单点实测"**，没有做正式的两点标定。
      建议接 GND 与 3.3 V 各测一次，把实测零点码值写回 `Frontend.c` 的
      `k_frontend_cal[]`（当前大/小量程零点为 1950 / 1321）。
- [ ] CH1 的噪声 spread ≈45（约 100 mV）明显大于 CH2 的 14（约 32 mV），
      两路共用同一 3.3 V 轨 → 疑似 **CH1 缺那颗 100 nF（ADC 节点 ↔ GND）**，
      它同时决定引脚低通拐点。待硬件确认。
- [ ] 负电压可测范围由硬件决定（大量程 −33 V、小量程 −3.0 V 为下限），软件无需改动；
      超过下限会被钳位到 0 附近，表现为 `DC` 贴边、`Vpp` 少一半。
- [ ] `adc.h` 未导出 `extern DMA_HandleTypeDef hdma_adc1;`，驱动里是自己 `extern` 声明的。
      如果哪天 CubeMX 改了 DMA 句柄名，这里要同步改。
- [ ] ADC 驱动引用 FreeRTOS API 后，链接器会把内核（含 32 KB 的 `ucHeap`）拉进来，
      RAM 从约 3 KB 涨到约 35 KB。这是启用任务调度后必然要用的堆；
      若长时间跑裸机，可加编译开关（`ADC_DRV_USE_FREERTOS 0/1`）省掉。
- [ ] 采样率上限受 ADC 采样时间限制：3 cycles 采样 + 12 cycles 转换 = 15 cycles @21 MHz
      ≈ 714 ns，理论上限约 1.4 MSPS；驱动对外限制到 1 MSPS。
