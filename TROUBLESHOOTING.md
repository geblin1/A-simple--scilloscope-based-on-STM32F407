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
| 9 | ADC2 只在复位时测一次 | 双 ADC 同步模式下读 `CDR` 高 16 位不可靠 |
| 10 | DMA 只搬了 1 个点 | 多 ADC 模式的 `HAL` 不置 `CR2.DMA`；`CCR.DDS` 被覆盖 |
| 11 | 按键只亮灭、不闪烁 | 代码里根本没有闪烁逻辑，且按键只在松手沿上报一次 |
| 12 | 串口助手收不到数据 | 板载 CH340 未接到 MCU，需外部跳线 |

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
使能 ADC2 后，第一帧的数是对的，之后就冻结在复位那一刻的值不再变化。

**根因**
双 ADC 规则同步模式下，靠读公共数据寄存器 `ADC->CDR` 的高 16 位来取从器件结果，
在**没有开 DMA 的情况下并不可靠**——第一次转换后就冻结了。

**解决**
自检场景不需要同步，改用**独立模式**逐路轮询：临时退出多 ADC 模式，分别启动、
分别读各自的 `DR`，最后恢复：

```c
saved_ccr = ADC->CCR;
ADC->CCR  = saved_ccr & ~(ADC_CCR_MULTI | ADC_CCR_DMA);   /* 退出同步模式 */
saved_exten = hadc1.Instance->CR2 & ADC_CR2_EXTEN;
saved_dma   = hadc1.Instance->CR2 & ADC_CR2_DMA;
hadc1.Instance->CR2 &= ~(ADC_CR2_EXTEN | ADC_CR2_DMA);    /* 回到软件启动，见问题 7 */

/* hadc1: Start + PollForConversion + 读 DR */
/* hadc2: Start + PollForConversion + 读 DR */

hadc1.Instance->CR2 |= (saved_exten | saved_dma);
ADC->CCR = saved_ccr;                                      /* 原样恢复 */
```

**经验**
`ADC->CDR`（`ADC123_COMMON` 基址 `0x40012300`，偏移 `0x08`）低 16 位是 ADC1、
高 16 位是 ADC2，**一次读取拿到两路、没有先后时差**——但它只在同步模式且转换真正发生时更新。
要用它就必须把 DMA/触发路径配全，否则退回独立模式更省心。

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

## 六、关键 HAL 源码事实（这次调试的真正收获）

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

---

## 七、当前代码结构

```
CMakeLists.txt                     ← User/ 自动收集源文件与头文件目录（问题 1）
User/
  BSP/
    Inc/  ADC_Drv.h                ← ADC 对外 API（业务层唯一需要包含的头文件）
          ADC_Test.h  ADC_DMA_Test.h  Encoder.h  Key.h  LED.h  SPI.h
    Src/  ADC_Drv.c                ← 唯一接触 ADC/TIM2/DMA2_Stream0 的实现
          Retarget.c               ← printf → USART1（重写 _write，问题 3）
          ADC_Test.c  ADC_DMA_Test.c  ← 纯 API 调用示例
          Encoder.c  Key.c  LED.c  SPI.c
Core/
  Src/adc.c   ← USER CODE BEGIN ADC2_Init 2：__HAL_ADC_ENABLE(&hadc2)（问题 8）
  Src/tim.c   ← USER CODE BEGIN TIM4_Init 2 / TIM8_Init 2：编码器模式启动（问题 4/5）
  Src/main.c  ← USER CODE 段：ADC_Drv_Init()、printf 头文件
```

**HAL 回调归属约定**
`HAL_ADC_ConvCpltCallback()` / `HAL_ADC_ErrorCallback()` 由 `ADC_Drv.c` 独占实现，
业务层不要再重复定义（会链接冲突）。

**DMA 缓冲区约束**
采集缓冲区必须放在 SRAM，**不能放 CCMRAM**——DMA1/DMA2 访问不到 CCMRAM
（F4 的 CCMRAM 只挂在 CPU 的 D-bus 上）。分配时用普通全局数组即可。

---

## 八、遗留 / 待办

- [ ] 按键闪烁（问题 11）目前只有方案，未实施。
- [ ] `adc.h` 未导出 `extern DMA_HandleTypeDef hdma_adc1;`，驱动里是自己 `extern` 声明的。
      如果哪天 CubeMX 改了 DMA 句柄名，这里要同步改。
- [ ] ADC 驱动引用 FreeRTOS API 后，链接器会把内核（含 32 KB 的 `ucHeap`）拉进来，
      RAM 从约 3 KB 涨到约 35 KB。这是启用任务调度后必然要用的堆；
      若长时间跑裸机，可加编译开关（`ADC_DRV_USE_FREERTOS 0/1`）省掉。
- [ ] 采样率上限受 ADC 采样时间限制：3 cycles 采样 + 12 cycles 转换 = 15 cycles @21 MHz
      ≈ 714 ns，理论上限约 1.4 MSPS；驱动对外限制到 1 MSPS。
