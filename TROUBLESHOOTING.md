# 调试问题复盘（Troubleshooting Log）

> 项目：基于 STM32F407ZGT6 的简易示波器（ADC 双通道同步采样 + 偏置前端 + ST7789 显示 + FreeRTOS 三任务）
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
| 25 | 任务化改造后编译报 `App_Scope_Loop` 未声明 | 接口重构删掉了旧主循环，`main.c` 的调用点没同步 |
| 26 | RUN 之后 LED 闪烁忽快忽慢（HOLD 时正常） | 界面任务画一帧几十毫秒、`vTaskDelayUntil` 立刻返回，低优先级监控任务被饿死 |
| 27 | HOLD 后按 KEY1 切通道，参数栏仍是原通道的数据 | 交接帧里只带了一份测量结果，采集时就绑定了当时的选中通道 |
| 28 | 上 FreeRTOS 后波形噪声明显变大（裸机版很干净） | 采集与刷屏从"串行"变成"并行"，SPI 突发在 3.3 V/地上打尖峰，经偏置电阻灌进 ADC 节点 |
| 29 | 波形一直左右抖动、周期信号不"钉"屏 | 自由采样没有触发点，每帧起始相位随机（是"抖"不是"噪"） |
| 30 | 打开触发后刷新率掉回 8 fps 量级 | 触发点只对齐到采样网格，波形每帧横向差 ±1 像素，行指纹全部失效 |
| 31 | 单极性 PWM / 逻辑信号怎么调电平都钉不住 | 默认电平 0 mV 在单极性信号幅度之外，AUTO 电平（信号中点）缺失 |
| 32 | 按 KEY4 开始采集后，按键/编码器要十几秒才响应一次 | 互斥量 give 只唤醒、不转移所有权，优先级更高的采集任务反复截胡总线锁，界面任务被饿死 |
| 33 | CH1 和 CH2 读数不一致 / 只有 CH1 标定过 | 标定表只按量程分、两通道共用，且没有设备端标定入口；CH2 从未实测过 |
| 34 | 手动两点标定后两路读数都约为实际值的一半 | `span` 被当成绝对码值当分母（少减了 zero），换算比例 = 码值差 / 绝对码值 |

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

**后续（10 月 23 日）：又改回逐通道**
分通道 V/div 重新实现：`App_State.vdiv_idx[2]`、`Frontend` 量程 API 逐通道、
状态栏并列显示两路 V/div 与两个量程字母、菜单行 `V/div C%u` 同时显示两路值。
这次用"把两个值都显示出来"解决"看起来一样却读数不同"的困惑，而不是牺牲灵活性。
详见 §15.12。

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
| （10 月 22 日更新）KEY1 在切测量通道的同时把触发源也切到该通道 | 否则会出现"选中的通道没被钉住、没选中的反而被钉住"（见问题 33 补充） |
| （10 月 23 日更新）V/div 又改回**逐通道独立**（`s_st.vdiv_idx[2]`），状态栏并列显示两个值、两个量程字母；菜单 `V/div` 行按当前通道改、行内并列显示两路值 | 用户需要两路不同量程（如 CH1 测 3.3 V 逻辑、CH2 测小信号）；"两个值都显示"足以避免"看起来一样却读数不同"，不必牺牲灵活性（见 §15.12） |

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

> 这一节按**实际在编译、实际在运行**的文件重写（10 月 22 日）。标了“历史 / 未调用”的是早期调试留下的，保留但不参与当前流程。

```
CMakeLists.txt                          ← 工程入口：User/ 自动收集 + FreeRTOS 内核/移植层 + 启停钩子
CMakePresets.json                       ← Ninja + arm-none-eabi 预设
STM32F407xx_FLASH.ld                    ← FLASH 1 MB / RAM 128 KB / CCMRAM 64 KB（标定占扇区 11）
startup_stm32f407xx.s                   ← 启动文件（向量表、Reset_Handler）
基于STM32F407ZGT6的简易示波器.ioc         ← CubeMX 工程；重新生成会改写 Core/（USER CODE 段保留）

cmake/
  gcc-arm-none-eabi.cmake               ← 工具链定义（CPU / FPU / 浮点 ABI）
  starm-clang.cmake                     ← 备用 clang 工具链
  stm32cubemx/CMakeLists.txt            ← CubeMX 管理的源码清单（不含 freertos_app.c / freertos_hooks.c）

tools/
  gen_font.py                           ← 生成 5×7 点阵字库（Lcd / Font 用）
  calc_frontend.py                      ← 按 KVL 重算前端偏置参数（问题 23 的模型）

ThirdParty/FreeRTOS/
  README.md
  Source/                               ← 内核：tasks.c queue.c list.c timers.c 等
  Source/portable/GCC/ARM_CM4F/         ← Cortex-M4F 移植层（port.c、heap_4.c）

User/
  BSP/                                  ← 驱动层：只有这里碰 HAL
    Inc/  ADC_Drv.h  Frontend.h  Lcd.h  Font.h
          EncoderDrv.h  Key.h  LED.h
          ADC_SelfCheck.h               ← 裸机自检（main.c 开关切换）
          ADC_Test.h  ADC_DMA_Test.h  Encoder.h    ← 历史测试 / 旧编码器，现未调用
    Src/  ADC_Drv.c                     ← 唯一接触 ADC / TIM2 / DMA2_Stream0 的实现
          Frontend.c                    ← 逐通道×逐量程标定、raw→mV、flash 扇区 11 读写
          Lcd.c  Font.c                 ← ST7789 SPI 屏 + 字库
          EncoderDrv.c                  ← TIM8 / TIM4 硬件编码器，四倍频折算成“格”
          Key.c                         ← TIM7 1 ms 中断里 20 ms 消抖、松手沿锁存
          LED.c                         ← 1 ms tick 驱动 500 ms 闪烁（中断里调用）
          Retarget.c                    ← printf → USART1（重写 _write）
          ADC_SelfCheck.c               ← 串口自检程序
          ADC_Test.c  ADC_DMA_Test.c  Encoder.c   ← 历史测试 / 旧编码器，现未调用
  View/                                 ← 视图层：只调 Lcd / Font，不含 HAL
    Inc/  ScopeView.h                   ← 布局常量、配色、各 *Info 结构 + WaveView_Data
    Src/  View.c                        ← 整屏换底（上电 / 换页）
          WaveView.c                    ← 增量刷新、脏行、行指纹、触发电平标记
          StatusBar.c  ParamBar.c       ← 状态栏 / 参数栏（字段级增量）
  Algorithm/                            ← 纯计算：不包含任何硬件头文件
    Inc/  Measure.h  Trigger.h  Fft.h
    Src/  Measure.c                     ← 峰峰值 / 有效值 / 平均值 / 频率
          Trigger.c                     ← 迟滞触发查找，返回 index + frac_q16（亚采样位置）
          Fft.c  FftTables.h            ← 256 点定点 FFT + 窗/旋转因子/log2 查表（表由 tools 生成）
  App/                                  ← 应用层：输入、状态机、编排
    Inc/  App_Scope.h  ScopeMenu.h  App_Rtos.h  ADC_NoiseTest.h
    Src/  App_Scope.c                   ← AcqStep / UiStep：采集、触发、测量、菜单、标定
          App_Rtos.c                    ← 三个任务 + 启动调度器
          ScopeMenu.c                   ← 220×170 设置弹窗（7 行）
          ADC_NoiseTest.c               ← 裸机噪声 A/B 对比（main.c 开关切换）

Core/
  Inc/  FreeRTOSConfig.h                ← 1 kHz 时基、8 个优先级、32 KB 堆、断言钩子
        adc.h  dma.h  spi.h  tim.h  usart.h  gpio.h  main.h
        stm32f4xx_hal_conf.h  stm32f4xx_it.h  freertos_app.h
  Src/
        main.c                          ← ADC_Drv_Init() → Frontend_Init() → App_Scope_Init() → App_Rtos_Start()
                                          （另有 APP_RUN_SELFCHECK / APP_RUN_NOISETEST 两个裸机开关）
        adc.c                           ← USER CODE：ADC2 要补 __HAL_ADC_ENABLE（问题 8）
        tim.c                           ← USER CODE：TIM4 / TIM8 编码器模式启动（问题 4 / 5）
        spi.c                           ← SPI1 模式 3、10.5 Mbit/s
        stm32f4xx_hal_timebase_tim.c    ← HAL 时基改用 TIM7（SysTick 让给 FreeRTOS）
        stm32f4xx_it.c                  ← DMA2_Stream0 / TIM7 等 IRQ；SVC/PendSV/SysTick 被宏改名
        freertos_app.c                  ← 手写的 RTOS 示例入口（队列 + 软件定时器），编译但未调用
        freertos_hooks.c                ← configASSERT / 栈溢出 / 堆耗尽钩子（死循环 + 现场变量）
        dma.c  gpio.c  usart.c          ← CubeMX 外设初始化
        stm32f4xx_hal_msp.c  system_stm32f4xx.c  sysmem.c  syscalls.c
```

**分层调用方向（单向，不许反向）**

`App → View（Lcd / Font）`、`App → Algorithm`、`App → BSP（ADC_Drv / Frontend / Key / EncoderDrv / LED）`。
`View` 与 `Algorithm` 都不包含 `adc.h` / `tim.h` / `FreeRTOS.h`，换屏、换采样器件时不用改它们。
只有 `App` 同时看到 FreeRTOS（互斥量、任务节拍）和业务状态。

**任务层与业务层的分工**

`App_Rtos.c` 只负责“谁、什么优先级、多少栈、什么节拍”；业务拆成
`App_Scope_AcqStep()`（采一帧 → 触发 → 测量 → 发布）与 `App_Scope_UiStep()`（处理一次输入 → 绘制 / 标定状态机）。
两个步进函数都**不创建任务、不按节拍阻塞等待**，所以同一套业务代码
裸机轮询与 RTOS 调度两用，任务层可整体替换而不动业务逻辑。

**HAL 回调归属约定**

`HAL_ADC_ConvCpltCallback()` / `HAL_ADC_ErrorCallback()` 由 `ADC_Drv.c` 独占实现，
业务层不要再重复定义（会链接冲突）。
`Core/Src/stm32f4xx_it.c` 里 CubeMX 生成的 `SVC_Handler` / `PendSV_Handler` / `SysTick_Handler`
在编译时被 CMake 宏改名成 `CubeMX_Unused_*`，把三个异常让给 FreeRTOS 移植层，
避免与 `port.c` 重复定义。

**DMA 缓冲区约束**

采集缓冲区必须放在 SRAM，**不能放 CCMRAM**——DMA1/DMA2 访问不到 CCMRAM
（F4 的 CCMRAM 只挂在 CPU 的 D-bus 上）。分配时用普通全局数组即可。

**文件是怎么进编译的**

- `User/**`：根 `CMakeLists.txt` 用 `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` 自动收集 `.c` / `.h`，
  并自动把各 `Inc` 目录加进 include path；新增文件不用改 CMake。
- `Core/Src`：由 `cmake/stm32cubemx/CMakeLists.txt` 的固定清单列出（CubeMX 生成）；
  `freertos_app.c` / `freertos_hooks.c` 与 FreeRTOS 内核由根 `CMakeLists.txt` 手工追加。
- 根 `CMakeLists.txt` 是 "generated only once"，CubeMX 重新生成代码不会覆盖它；`User/` 也不归 CubeMX 管。

---

## 十、遗留 / 待办

- [x] ~~按键闪烁（问题 11）~~：**已由 TIM7 的 1 ms 中断驱动**（`LED_Tick()`）——
      **每 500 ms 翻转一次，RUN 与 HOLD 相同**。节奏只由硬件定时器决定，
      界面刷屏再慢也不会让灯忽快忽慢（踩过的坑见问题 26）。
- [x] ~~双通道的 V/div 共用同一档~~：10 月 23 日已改回**逐通道独立**（`vdiv_idx[2]`），
      菜单 `V/div C%u` 行、状态栏两值两字母，详见 §15.12。前端量程与跳线乙仍按各自档位对应。
- [x] ~~前端标定还只是"设计值 + 单点实测"~~：**已实现逐通道 × 逐量程两点标定**
      （接 GND 记零点、接 3.3 V 记满点；KEY1 选通道、KEY4 采样，结果存 flash 扇区 11）。
      上电 `Frontend_Init()` 读回，校验失败才退回设计默认值（1950 / 1321）。见第十四节。
- [x] ~~CH1 的噪声 spread ≈45（约 100 mV）明显大于 CH2 的 14（约 32 mV）~~
      **2026-10-23 用户已补装 CH1 的 100 nF（ADC 节点 ↔ GND）**，待上板用噪声测试模式
      （`APP_RUN_NOISETEST`）复查两路 spread 是否接近。
- [ ] 负电压可测范围由硬件决定（大量程 −33 V、小量程 −3.0 V 为下限），软件无需改动；
      超过下限会被钳位到 0 附近，表现为 `DC` 贴边、`Vpp` 少一半。
- [ ] `adc.h` 未导出 `extern DMA_HandleTypeDef hdma_adc1;`，驱动里是自己 `extern` 声明的。
      如果哪天 CubeMX 改了 DMA 句柄名，这里要同步改。
- [x] ~~ADC 驱动的 FreeRTOS 适配~~：驱动内部按 `xTaskGetSchedulerState()` 自动分支，
      不再需要编译开关。当前（含 640 点触发采集、调度修复、逐通道标定、标定分母修复、
      FFT 频谱与分通道 V/div）实测
      **RAM 59 688 B (45.54 %)**、**FLASH 70 088 B (6.68 %)**；其中 `ucHeap` 32 KB、
      三个任务实占约 6.5 KB，触发功能的 640 点缓冲另占约 3.8 KB。
- [ ] 若要继续压 RAM：`configTOTAL_HEAP_SIZE` 可从 32 KB 降到 16 KB（当前余量充足），
      但会让"堆不足"从不可能变成需要留意，建议先跑一段时间看 `g_monHeapFree` 再定。
- [ ] 采样率上限受 ADC 采样时间限制：3 cycles 采样 + 12 cycles 转换 = 15 cycles @21 MHz
      ≈ 714 ns，理论上限约 1.4 MSPS；驱动对外限制到 1 MSPS。

---

## 十一、FreeRTOS 任务化（10 月 21 日）

### 25. 从裸机主循环改成三任务

**为什么要改**：裸机主循环里"采集 → 测量 → 绘制"是串行的。一次整屏重画要几十毫秒，
这段时间 ADC 完全没人管；慢时基（1 kSa/s 时当时一帧 320 点 ≈ 320 ms；触发改成
640 点后约 640 ms）+ 固定 60 ms 超时，还会整帧判超时。任务化后采集由独立任务按节拍驱动，**绘制再慢也抢不走采样窗口**。

**任务划分**

| 任务 | 优先级 | 栈 | 节拍 | 职责 |
| :--- | :--- | :--- | :--- | :--- |
| `acq` | 4 | 1.0 KB | 20 ms 起 | 采一帧（640 点）→ 触发搜索取窗口 → 两路测量 → 发布新帧 → 让出 1 tick |
| `ui` | 3 | 2.5 KB | 10 ms | 读按键/编码器 → 改状态 → 取新帧 → 重绘（含标定状态机） |
| `mon` | 1 | 0.5 KB | 500 ms | 堆余量与两个任务的栈高水位（LED 心跳在 1 ms 中断里） |

**采集优先级必须高于界面**：LCD 是 SPI 阻塞发送，界面任务一跑就是几十毫秒。
采集任务优先级更高，就能在 DMA 完成中断给出信号量的瞬间抢占界面，
先把新帧发布出去、再让界面慢慢画。反过来（界面优先）采样率会退化成"看绘图速度"。

**数据交接：只锁一帧，锁内不碰硬件**
`Scope_Frame`（两路已标定 mV + 测量结果 + 采样率 + 序号）由互斥量保护：
采集任务锁内只做两次 `memcpy`（各 1280 B）+ `seq++` 就立刻释放；
界面任务锁内比较 `s_frame.seq != s_view.seq`，不同才拷到自己的 `s_view`。
两侧都**不在锁内操作硬件**（不发 SPI、不启停 DMA），临界区只有几微秒；
互斥量带优先级继承，不会优先级反转。
`s_st`（档位、运行状态）只由界面任务写、采集任务读，全是 8/32 位对齐标量，
单次读写天然原子，**不加锁**——加了反而会把几十毫秒的绘制拖进采集路径。

**踩的坑**：把 `App_Scope_Loop()` 拆成 `App_Scope_AcqStep()` / `App_Scope_UiStep()`
之后，`main.c` 的 `while(1)` 里还留着旧调用：

```
main.c:158:5: error: implicit declaration of function 'App_Scope_Loop'
              [-Wimplicit-function-declaration]
```

删掉即可。教训：**改公共 API 先全工程搜旧符号**；C99 下隐式声明是错误而不是警告，
编译器等于已经把"这里有个不存在的函数"喊出来了。

**验证方法（不靠肉眼）**

1. **符号必到**：`nm` 里要有 `App_AcqTask`/`App_UiTask`/`App_MonitorTask`、
   `xTaskDelayUntil`、`prvIdleTask`、`ucHeap`（32 KB），且**无未定义符号**。
2. **中断向量必须归 FreeRTOS**：读 `.isr_vector` 的 0x2C / 0x38 / 0x3C 三项，
   应分别指向 `SVC_Handler` / `PendSV_Handler` / `SysTick_Handler`
   （本次为 0x08006b00 / 0x08006d80 / 0x08006de8）。
   调度器一启动 SysTick 就是内核心跳——**这一项不对，整个 RTOS 都是死的**。
   `.map` 里 `CubeMX_Unused_SVC_Handler` 被 `--gc-sections` 回收，
   说明 `CMakeLists.txt` 的符号改名仍在生效。
3. **栈够用**：看 `.su` 文件，Top 是 `Measure_Analyze` 288 B、`ParamBar_Draw` 128 B、
   `Frontend_RawToMv` 104 B；1 KB 采集栈与 2.5 KB 界面栈都有数倍余量，
   运行期再用 `uxTaskGetStackHighWaterMark()` 复核。
4. **任务里没有阻塞延时**：全工程搜 `HAL_Delay`，只应剩 `ADC_Drv_DelayMs()` 的裸机分支
   与 `Lcd_Init()` 的复位序列（只在调度器启动前跑一次）。

**稳定性措施的落点**：`configCHECK_FOR_STACK_OVERFLOW = 2` →
`vApplicationStackOverflowHook()`；`configUSE_MALLOC_FAILED_HOOK = 1` →
`vApplicationMallocFailedHook()`。两个钩子都在 `Core/Src/freertos_hooks.c`，
做"关中断 + 记录现场 + 停机"，不会带着坏状态继续跑。

**实测资源**（迁移并加上"采集/刷屏互斥"后）：RAM 52 008 B (39.68 %)、FLASH 52 532 B (5.01 %)；
其中 `ucHeap` 32 KB，三个任务实占约 6.5 KB（1.0 + 2.5 + 0.5 KB 栈 + TCB）。

> 10 月 22 日加入触发、调度修复与逐通道标定后为 **RAM 56 088 B / FLASH 64 120 B**；
> 10 月 23 日修正标定换算分母后 FLASH **64 428 B**；同日晚加入 AUTO 自动设置后
> **RAM 56 088 B / FLASH 65 380 B（6.24 %）**；加入 FFT 频谱后为
> **RAM 59 680 B / FLASH 69 660 B（6.64 %）**；再加入分通道 V/div 后为
> **RAM 59 688 B / FLASH 70 088 B（6.68 %）**（见第十五节 §15.9）。

---

### 26. RUN 之后 LED 闪烁忽快忽慢

**现象**：烧录后不按运行键时，LED 以稳定的约 1 Hz 闪烁；一按 KEY4 开始采集，
闪烁立刻变得极不规律——有时好几秒不动，有时连着抖好几下。

**根因：任务饥饿**
1. LED 心跳原本由监控任务 `mon`（优先级 1，最低）每 500 ms 翻转一次；
2. 界面任务 `ui`（优先级 3）每次迭代画一帧，`App_Scope_UiStep()` 里的
   `HAL_SPI_Transmit()` 是**忙等轮询**，不会主动让出 CPU，一帧就要几十毫秒；
3. 而 `vTaskDelayUntil(&last, 10 ms)` 的周期只有 10 ms，早就被超期，于是它
   **每个迭代都立刻返回**——界面任务实际上退化成一个"永不阻塞的纯计算循环"；
4. 更低优先级的 `mon` 和空闲任务几乎拿不到 CPU，只能趁界面任务被采集任务
   抢占时蹭到一点，于是 `mon` 的节拍被拉长、又成批追赶，LED 就忽快忽慢。

**为什么 HOLD 时反而正常**：不采集时界面没有新帧要画，`App_Scope_UiStep()`
很快返回，`vTaskDelayUntil` 能真正等满 10 ms，`mon` 按时执行，闪烁自然稳定。

**解决（双管齐下）**
1. **把心跳搬进硬件中断**：新增 `LED_Tick()`，由 TIM7 的 1 ms
   中断调用（与 `Key_Tick()` 同一个回调）。函数里只有寄存器写和一个静态计数，
   不阻塞、不受任务调度影响，节奏恒定。
   （中途为了贴合"运行指示"曾改成"HOLD 闪、RUN 常亮"，后来按需求恢复为
   **一直每 500 ms 翻转**——关键从来不是亮/闪的语义，而是**节奏由谁驱动**。）
2. **界面任务主动让出 CPU**：每次迭代末尾 `vTaskDelay(1)`，只损失约 3% 的绘制时间，
   换来 `mon` 与空闲任务按时执行（`g_monHeapFree`、栈高水位不再长期不刷新）。

**验证（反汇编）**

```
80074e0: bl  Key_Tick
80074e4: bl  LED_Tick
```

`LED_Tick` 内部没有依赖运行状态的分支：`count++` → `cmp #500` → 不足则直接返回，
否则计数清零并 `bl LED_Turn`。

**经验**
- **低优先级任务被饿死，症状通常先出现在"它负责的那点小输出"上**（LED、日志、
  统计值），而不是明显的卡死。这类输出一旦不稳定，先怀疑调度而不是硬件。
- **`vTaskDelayUntil` 救不了超期的循环**：周期比执行时间短时它等价于 `while(1)`，
  必须有别的让出点（延时、等信号量，或给低优先级任务留出时间片）。
- **心跳/看门狗类的指示不要放在可能被饿死的任务里**，交给硬件定时中断最可靠。

---

### 27. HOLD 后按 KEY1 切通道，参数栏仍是原通道的数据

**现象**：按 KEY4 暂停后，两条波形都冻结在最后一帧（这部分是对的）；但下方参数栏的
`F / T / Vpp / RMS / DC` 只显示"暂停那一刻所测通道"的数值，按 KEY1 切到另一通道也不变。
运行（RUN）时切换通道则完全正常。

**根因**：交接帧里只有**一份**测量结果。`App_Scope_AcqStep()` 按当时的
`s_st.measure_ch` 只测一路：

```c
Measure_Analyze((ch == 1U) ? s_mv1 : s_mv2, APP_CAP_LEN, sps, &meas);
```

所以帧里那份结果从生成起就绑定了"采集瞬间选中的通道"。KEY1 只改了**显示选择**，
没有另一路的数据可换；HOLD 时没有新帧进来，这个错位就固定显形了。
（RUN 时下一帧立刻按新通道重测，看不出问题。）

**解决**：让每一帧**同时带上两路的测量结果**。

1. `Scope_Frame` 的 `Measure_Result meas;` → `Measure_Result meas[2];  /* [0]=CH1, [1]=CH2 */`
2. `App_Scope_AcqStep()` 每帧测两次（顺序调用，栈峰值不变）：
   ```c
   Measure_Analyze(s_mv1, APP_CAP_LEN, sps, &meas[0]);
   Measure_Analyze(s_mv2, APP_CAP_LEN, sps, &meas[1]);
   ```
3. `App_DrawBars()` 按当前通道取指针：
   ```c
   const Measure_Result *m = &s_view.meas[(s_st.measure_ch == 1U) ? 0U : 1U];
   ```
4. `App_PublishFrame()` / `App_PullFrame()` 跟着拷两个元素。

**代价**：每帧多一次 `Measure_Analyze()`（320 点整数运算，栈 288 B），
实测 RAM 51 952 → 52 000 B、FLASH 52 396 → 52 400 B，帧率无可测变化。

**经验**
- **"显示的数据"和"显示的选择"必须是两份独立的东西**：只要数据是"按当时的选中项
  算出来的"，一旦停止刷新（HOLD / 冻结 / 回放），切换选择就必然指向旧数据。
  把多路结果一起带上，显示层只负责选指针，任何时刻切换都自洽。
- 这类 bug 在 RUN 下会被下一帧自动掩盖，**只有冻结状态才暴露**——所以"暂停/回放/冻结"
  类功能必须单独测一遍交互，不能只测运行态。

### 28. 上 FreeRTOS 后波形噪声明显变大（裸机版很干净）

**现象**：同一套前端、同样的采样率、同一块板子。裸机版本测量时波形非常干净；
改成三任务调度后，两条波形都出现明显噪声（幅度抖动、毛刺）。第一反应是怀疑 LED 或 RTOS。

**先排除了两个嫌疑**

- **不是 LED**：LED 心跳是 **1 Hz** 方波（每 500 ms 翻转一次）。它造成的干扰只可能是
  "每 500 ms 出现一个台阶"，不可能是一条连续的噪声带。把 `LED_Tick()` 那行临时注释掉
  再烧一次，噪声一点没变——证伪。
- **不是"RTOS 本身"**：中断响应、上下文切换的开销都在微秒量级，不改变任何模拟量。
  RTOS 不会"凭空产生噪声"。

**真正变的是"采样与刷屏的时间关系"**

| | 裸机版 | RTOS 版 |
|---|---|---|
| 采集 320 点（40 kSa/s，当时；触发后为 640 点 = 16 ms） | 8 ms | `acq` 任务每 20 ms 启一帧 |
| 刷屏 | 采完之后才开始画（约 117 ms） | `ui` 任务同时在画（30 ~ 50 ms） |
| 两者关系 | **永不重叠** | **必然重叠** |

关键在于：**DMA 搬运和 SPI 发送都是硬件外设自己跑的**，跟"哪个任务占着 CPU"没有关系。
只要两者在时间上重叠，整屏 SPI 突发（121 ~ 154 KB，10.5 Mbit/s，约 117 ms）就会在
3.3 V 与地上打出电流尖峰；而前端偏置网络是 **500 Ω 戴维南内阻直连 3.3 V**，
尖峰正好从这里灌进 ADC 节点。采样窗口只有 3 cycles ≈ 143 ns，落在尖峰上就被采进去。
裸机版天然无害，纯粹因为它**先采完再画**，两件事从不重叠。

**解决：一把"总线互斥量"**

`User/App/Src/App_Scope.c` 加 `static SemaphoreHandle_t s_bus_lock;`，把"采集"和"刷屏"
互斥开（`APP_SERIALIZE_ACQ_DRAW` 置 0 可退回并行状态做对比）：

- `App_Scope_AcqStep()`：`App_BusLock() → App_Capture() → App_BusUnlock()`
- `App_Scope_UiStep()`：菜单绘制块、主界面绘制块**各自整体**包进 `App_BusLock()/App_BusUnlock()`

反汇编确认两个步进函数都正确包住：

```
App_Scope_AcqStep: bl App_BusLock / bl App_Capture / bl App_BusUnlock / bl App_PublishFrame
App_Scope_UiStep : bl App_BusLock / bl App_DrawWave / bl App_DrawBars / bl App_DrawMenu / bl App_BusUnlock
```

互斥量带优先级继承（采集优先级 4、界面 3），不会出现优先级反转。

**实测**：改完直接烧录（没动硬件），双通道波形都恢复稳定。
噪声根因确认就是"采集与刷屏并行"，与 LED、与 RTOS 调度本身都无关。

**代价（用帧率换信噪比）**

慢时基下界面要等一次采集：1 kSa/s 时当时一帧 320 ms、刷新降到约 3 fps（触发把采集
改成 640 点后约 640 ms、约 1.5 fps；原来约 26 ~ 43 fps）。
快时基（≤ 100 kSa/s）几乎无感，因为采集本来就比绘制短得多。
对示波器来说这笔交易是划算的——**画面漂亮但数据是脏的，不如画面慢一点但数据可信**。

**附带：怎么量化确认**

`User/App/Src/ADC_NoiseTest.c` 是个临时诊断（`Core/Src/main.c` 里
`#define APP_RUN_NOISETEST` 置 1 启用），做 A/B 两组对照：

- **A 组**：只采集、不绘制；
- **B 组**：采集过程中调用 `View_DrawFrame()` 整屏重画，制造并行。

两组各跑 6 轮，各 320 点，打印两路的 `spread avg / max`（码值与 mV）。
判读口径：

- **B 明显大于 A** → 刷屏耦合，正是本文这个根因；
- **A 本身就很大** → 与 LCD 无关，问题在前端电路 / 地 / 供电；
- **A、B 都小** → 你看到的"噪声"其实是无触发导致的相位游走，那是"抖"不是"噪"。

**经验**

- **上 RTOS 后要重新审视"硬件时序"，不能只看 CPU 归属**。任务优先级保证的是
  "谁用 CPU"，保证不了"两个外设不同时工作"——DMA、SPI、ADC 都是自动跑的，
  必须显式加锁或有意识地串行化。
- **"之前好好的，加了 X 就坏了"，一定要把 X 拆开看**。这次 X 里真正起作用的是
  "并行"，而不是 LED、也不是 RTOS 本身。先做能**证伪**的对照实验（注释 LED、
  置 0 开关），比凭直觉改代码快得多。
- **这类问题的典型特征是"只有加了并行机制才出现"**：裸机工程师习惯的"先采后画"
  本身就是一种隐式互斥，一旦拆成任务，这层保护就没了。
- **还有一条待办（已实现）**：之前没有触发，每帧起始相位随机，周期信号在屏幕上会缓慢游走。
  这是"抖"而不是"噪"，噪声修好之后单独立项，现已实现（见第十二节）。

---

## 十二、边沿触发 + 预触发（10 月 21 日）

### 29. 波形一直左右抖动，周期信号不"钉"屏

**现象**

问题 28 的噪声修好后，波形干净了，但仍在**左右缓慢游走**。周期信号（如 1 kHz 正弦）
每一帧的起始相位都不同，屏幕上看起来"抖"，读周期/相位根本没法读。
这不是噪声——噪声是线变粗，抖动是整条线在水平方向平移。

**根因**：自由采样。每帧从任意时刻开始采一整窗（当时 320 点，后改成 640 点带预/后触发），
帧与帧之间没有共同的"时间零点"，所以周期信号在屏幕上的横向位置每帧都不同。

**方案：中心触发 + 半屏预触发**

| 参数 | 取值 | 理由 |
| :--- | :--- | :--- |
| 实际采集 | **640 点** = 320 显示 + 160 预触发 + 160 触发后 | 触发点放屏幕正中，两侧都有波形 |
| 搜索窗口 | `k ∈ [160, 479]` | 保证 `k−160 ≥ 0` 且 `k+160 ≤ 639`，显示窗完整落在采集缓冲内 |
| 迟滞 | `触发通道的 V/div ÷ 20`（至少 1 mV） | 单阈值时噪声在电平附近反复穿越，触发点每帧跳一个采样点；滞回把"抖"压到 ≤1 px |
| 触发电平步进 | `触发通道的 V/div ÷ 10` | 每个大格分 10 步，手感与 V/div 档位一致；分通道 V/div 后按触发源那一路算 |
| 电平范围 | ±5 格 | 再远就永远碰不到波形，标记只会贴在边框上 |
| 捕获超时 | `640×1000/sps + 100 ms`（上限 1500） | 慢时基 1 kSa/s 时一帧 640 ms，固定超时会每帧误判 |

**关键代码**

- `User/Algorithm/Src/Trigger.c`：`Trigger_Find()` 纯整数，扫描第一个满足
  `mv[k] ≤ 电平−迟滞 && mv[k+1] ≥ 电平+迟滞`（上升沿）的采样区间，返回
  `Trigger_Point { index, frac_q16 }`——`index` 是穿越前一刻的下标，`frac_q16`
  是穿越时刻在 `index..index+1` 之间的 Q16 位置。没找到返回 `index = -1`。
  下降沿反向。无 HAL/FreeRTOS 依赖。
- `User/App/Src/App_Scope.c`：
  `App_Capture()` 采 640 点 → 逐点 `Frontend_RawToMv()` → 按触发源选通道 →
  `Trigger_Find()` → `App_ResampleWindow()` 用 `frac_q16` 对两路做就地线性插值，
  把显示窗对齐到真实穿越时刻（`win_start = 0`）；没找到时 `win_start = 160`、
  退回自由滚动。`App_PublishFrame()` 只发布这一窗 320 点，界面层完全不知道
  触发搜索的存在。测量也改用这一窗（`Measure_Analyze(&s_mv1[win_start], ...)`），
  测量结果与屏幕上的波形严格对应。

**没触发怎么办：回退，不冻结**

找不到穿越点时**不冻屏**，而是照常显示中间 320 点（自由滚动），并把电平线画成**红色虚线**；
锁定成功时画**灰色虚线**。这样"没触发"在屏幕上是可见的，而不是一个需要猜的黑盒。
代价是红色虚线的存在让"锁定指示"只能靠颜色——但状态栏 240 px 宽已被
`CH / RATE / TIME / MODE / RUN / LIMIT` 占满，加文字必然挤掉现有信息，颜色是零成本方案。

**资源占用（Debug 构建实测）**

| 指标 | 触发功能加入前 | 加入后 |
| :--- | ---: | ---: |
| FLASH | 52 532 B（5.01 %） | **55 048 B（5.25 %）** |
| RAM | 52 008 B（39.68 %） | **55 864 B（42.62 %）** |

RAM 增量约 3.8 KB 来自 640 点缓冲：`s_raw[640]`（4 B/点）+ `s_mv1[640]` + `s_mv2[640]`，
比原来的 320 点版本正好翻倍。CCMRAM 仍为 0——DMA 缓冲区必须在 SRAM。
亚采样插值（`Trigger_Point` + `App_ResampleWindow()`）另加约 0.6 KB FLASH；
AUTO 电平（`App_AutoLevel()`）再 +0.2 KB，触发功能最终为 55 048 B（5.25 %）。
之后调度修复（问题 32）+256 B、逐通道标定（问题 33，含 HAL flash 驱动）+约 8.8 KB，
标定分母修复 +308 B、AUTO 自动设置 +952 B、FFT 频谱 +4 280 B、分通道 V/div +428 B，
**当前为 FLASH 70 088 B（6.68 %）/ RAM 59 688 B（45.54 %）**，完整增量表见 §15.9。

**代价**

- 一帧采集时长翻倍（`640/sps`）：1 kSa/s 时约 640 ms/帧，慢时基下界面响应变慢。
  这是拿响应速度换波形稳定，与问题 28 的"用帧率换信噪比"是同一类取舍。
- 触发搜索本身 O(320) 次整数比较，加上亚采样重采样（2×320 次插值），相对 DMA 等待可忽略。

**验证状态（重要）**

- 已通过 `arm-none-eabi-gcc`（`-Wall -O0 -g3`）完整构建，**零警告**，链接产物
  `.elf`/`.map` 正常，内存占用如上表。
- **尚未实机验证**：本次改动只做了编译与代码审查，没有烧录到板子上。
  上板后建议优先确认四件事：
  1. 默认（CH1、上升沿、0 mV 电平）下 1 kHz 正弦是否稳定锁定（灰色虚线、波形不动）；
  2. 触发电平扫到波形范围外时，是否退化为红色虚线 + 自由滚动（不黑屏、不冻屏）；
  3. 菜单 7 行弹窗（220×170，当时为 5 行 220×130）是否把波形区盖得过多、退出后是否整片重画干净；
  4. 慢时基（100 ms/div、1 kSa/s）下按键是否明显变迟钝（预期约 640 ms 一次）。

**经验**

- **"抖"和"噪"要分开诊断**：噪是线变粗（垂直方向随机），抖是整条线平移（水平方向随机）。
  前者查采样环境（问题 28），后者查时间基准（本节）。
- **触发功能必须让"锁定状态"可见**。示波器用户看到波形动，第一反应是"信号有问题"；
  如果软件不告诉他"这帧没触发"，他会一直调电平却不知道为什么没用。
- **回退路径比成功路径更重要**：触发失败时如果冻屏或黑屏，用户会以为程序死了。
  回退到自由滚动 + 视觉提示，是"失败也要可用"。

### 30. 打开触发后刷新率掉回 8 fps 量级

**现象**

触发功能加上之后，波形确实"钉"住了，但屏幕刷新明显变慢，接近问题 22 那个
整屏重画时代的水平（约 8~10 fps）。

**根因：触发点只对齐到了采样网格**

`Trigger_Find()` 一开始找到的是"第一个越过电平的采样点 k"。而真实的穿越时刻
落在采样点 `k−1` 与 `k` 之间，这个亚采样偏移（相对一个采样周期的比例）由
采样时钟与信号频率的相位差决定，**每帧都不一样**。于是显示窗的起点每帧平移
不到一个采样点，屏幕上的波形就横向抖动 ±1 像素。

对增量刷新来说这是致命的：行指纹（FNV-1a）是对整行 320 个像素算的，
一像素的横向平移会让曲线穿过的**每一行**像素值都变化，指纹全部不匹配，
于是每帧把整条波形重推一遍（72 行 ≈ 46 KB、满屏 190 行 ≈ 122 KB），
再加上参数栏 17.9 KB 和 16 ms 采集，单帧回到 50~110 ms。

**方案：亚采样插值 + 重采样**

1. `Trigger_Find()` 改为返回 `Trigger_Point { index, frac_q16 }`：
   `frac_q16 = (level − mv[index]) / (mv[index+1] − mv[index]) × 65536`（Q16 定点，
   用 int64 中间运算避免小信号时溢出）。
2. `App_ResampleWindow()` 把 320 点显示窗按 `frac_q16` 线性插值重采样：
   输出第 i 点 = `buf[index−160+i] + (buf[index−159+i] − buf[index−160+i]) × frac`，
   于是第 160 点恰好落在真实穿越时刻（屏幕中线）。
3. 帧与帧之间只剩 ADC 噪声差异，行指纹绝大多数命中，刷新率恢复到正常水平；
   顺带触发精度从"一个采样周期"提高到亚采样，波形真正钉死。

**代价与边界**

- 每帧多做 2×320 次 int64 乘除（约 0.2~0.4 ms @168 MHz），相对 DMA 等待可忽略。
- 没触发（红虚线）时不做插值，仍是自由滚动，刷新率与旧版一致。
- 插值假设相邻两点之间是直线：信号频率远低于采样率时足够好；
  1 kSa/s 慢时基下信号本身变化缓慢，插值误差远小于 1 像素。

**验证状态**：只做了编译 + 代码审查，**未上板**。上板重点看：锁定后电平线
为灰色时波形是否完全静止；红虚线时是否照常滚动；40 kSa/s 下帧率是否回到
20 fps 以上。

### 31. 单极性 PWM / 逻辑信号怎么都"钉"不住

**现象**

把探头接到 F103 输出的 3.3 V PWM 上，能看到方波，但波形一直自由滚动、中间是
未触发颜色的虚线（本应是红色；本机面板 R/B 顺序不同，实际显示偏青/偏蓝）。
在菜单里把 `Trig level` 从 0 mV 往上调，调进 0~3.3 V 之间才会锁定，调回 0 mV 又失效。

**根因：默认触发电平落在单极性信号的幅度之外**

`App_Scope_Init()` 把 `trig_level_mv` 初始化为 0。上升沿的判定条件是
"前一个采样点 ≤ 电平−迟滞，后一个采样点 ≥ 电平+迟滞"。单极性 PWM 最低就是 0 V
（逻辑 0），永远不会低于 `0 − 迟滞`，所以每个 640 点窗口都找不到触发点，
`state` 一直是 `APP_TRIG_STATE_MISSING`，画面退回自由滚动。

这不是 bug，而是"默认值只对双极性/带负偏置信号友好"的设计缺口：正弦波接上时
信号通常自然跨 0；PWM 却不跨 0。

**方案：AUTO 电平（信号中点重试）**

1. `App_AutoLevel()`（`User/App/Src/App_Scope.c`）扫一遍整段 640 点，取
   `(最小值 + 最大值) / 2` 作为候选电平；对 0…3.3 V PWM 就是约 1.65 V。
2. `App_Capture()` 先用菜单里的电平找触发点；找不到时用 AUTO 电平再找一次，
   命中则把该电平写回 `s_st.trig_level_mv`——菜单数值、屏幕电平线一起跟过去。
3. 仍然找不到（纯直流、边沿方向反了、幅度小于迟滞带）才回退自由滚动。

**代价与边界**

- 每帧多一次 640 点扫描（2 次比较/点），只在"按设定电平没找到"时发生，
  正常锁定时零开销。
- AUTO 只解决"电平不在幅度内"这一类失败；`Trig edge` 选反、`Trig src` 选错、
  幅度低于迟滞带仍会保持未触发。
- 纯直流信号没有穿越点，AUTO 取到的中点是直流值，仍然找不到穿越点，照常自由滚动。
- 中点对方波/三角波足够好；占空比很极端或带过冲的信号仍可手动微调电平。

**验证状态**

编译通过（`-Wall` 零警告）。上板重点看：3.3 V PWM 默认设置下是否直接锁定（灰线）；
把 `Trig level` 挪到幅度外后是否自动跟回中点；纯直流输入是否仍自由滚动。

**经验**

- **默认值要覆盖"最常见的错误接法"**：用户接上就期待稳定波形，默认 0 mV 对
  有偏置的信号合理，对单极性信号是陷阱；AUTO 在不增加菜单复杂度的前提下兜底。
- **自动行为必须可见**：AUTO 写回菜单值，用户能看到"电平自己变了"，否则会
  怀疑是 bug。文档里单独列一条 FAQ。
- **颜色问题要单独解释**：本机面板 R/B 反序，红色虚线实际偏青/偏蓝（见
  `LCD_UI_DESIGN.md`），判断只看"灰 = 锁定，非灰 = 未触发"。

---

## 十三、任务调度与总线锁（10 月 21 日）

### 32. 按 KEY4 开始采集后，按键/编码器要十几秒才响应一次

**现象**

上电停在 HOLD 时按键、编码器都正常；一按 KEY4 开始采集，按键和编码器要过十几秒
才响应一次，几乎没法操作。慢时基（1 kSa/s）时更明显。

**根因：互斥量 give 只唤醒、不转移所有权，高优先级采集任务把锁"截胡"**

界面任务和采集任务通过 `s_bus_lock` 互斥（问题 28 的采样/刷屏串行化）。采集任务
优先级是 `tskIDLE_PRIORITY+4`，界面任务只有 `+3`。FreeRTOS 的 `xSemaphoreGive()`
在互斥量上**只把等待者从阻塞态变成就绪态**，并不把所有权交给它；当前更高优先级的
采集任务会继续运行，在下一轮 `App_Scope_AcqStep()` 开头 `App_BusLock()` 时立刻
把计数=1 的锁重新取走。界面任务醒来再取时计数已是 0，只好继续等——只有采集任务
**真正阻塞**（`vTaskDelayUntil` 进入等待）时，界面才能抢到锁。

触发功能把采集从 320 点改成 640 点后，一帧 = 16 ms 采集 + 1280 次 `Frontend_RawToMv`
换算 + 触发查找/重采样 + 两路测量 + 发布 ≈ 20 ms 以上，正好吃掉 20 ms 的周期：
`vTaskDelayUntil` 几乎每帧立即返回，采集任务不让出 CPU，界面任务被长期饿死。
按键/编码器只在 `App_Scope_UiStep()` 开头读，于是要隔十几秒才被处理一次。

**方案：采集每帧让出 1 tick + 界面取锁有上限**

1. `User/App/Src/App_Rtos.c` 的 acq 循环：`App_Scope_AcqStep()` 返回非 0 后
   `vTaskDelay(1U)` 显式让出 1 个 tick，保证界面任务每帧都有机会拿到锁。
2. `User/App/Src/App_Scope.c`：新增 `App_BusLockWait(timeout)`，界面任务取锁改为
   最多等 `APP_UI_BUS_WAIT_MS`（20 ms）；新增 `s_draw_pending` 与 `App_UiDrawFrame()`，
   拿不到锁就保留"欠一帧"状态、下一轮再画，不再用 `portMAX_DELAY` 死等。
3. 绘制仍全程持锁，问题 28 的"采样期间不刷屏"约束不变。

**代价与边界**

- 每帧多 1 ms 让出（40 kSa/s 下约 5 %）；采集节拍由 16~20 ms 变成 17~21 ms，可接受。
- 界面最多等 20 ms；慢时基（1 kSa/s 一帧 640 ms）下界面会"少画几帧"，但按键/编码器
  每轮都先扫描，不会被 640 ms 的采集拖住。
- 这只解决"界面拿不到锁"的饿死；若界面自身绘制超过 20 ms，表现为掉帧而不是卡输入。

**验证状态**

编译通过（`EXIT=0`、`-Wall` 零警告），当时 FLASH **55 304 B / 1 MB（5.27 %）**、
RAM **55 864 B / 128 KB（42.62 %）**，CCMRAM 0（当前 70 088 B / 59 688 B，见第十五节）。**未上板**，上板重点看：
上电按 KEY4 后立刻转编码器/按键是否马上响应；HOLD/RUN 反复切换是否跟手；
慢时基下按键是否仍不卡。

**经验**

- **FreeRTOS 的 give 不是 handoff**：唤醒等待者 ≠ 把锁交给它。高优先级任务若在
  give 后不阻塞，就会把锁再抢回去；低优先级等待者只有在高优先级任务真正阻塞时
  才能拿到锁。要么让高优先级任务每轮真正延时（本次做法），要么用队列/任务通知
  显式传递"该谁画"。
- **界面任务对总线锁的等待必须有上限**，并且"欠帧"要作为显式状态保留，否则跳过
  一帧就等于丢一帧、按键也可能跟着丢。
- **任务周期要留余量**：一帧处理时间逼近任务周期时，`vTaskDelayUntil` 不再提供
  任何让出，必须显式 `vTaskDelay(1)`。这与问题 26 是同一类调度饿死，只是这次
  饿死发生在总线锁上。

---

## 十四、逐通道标定（10 月 21 日）

### 33. CH1 和 CH2 读数不一致，而且只有 CH1 标定过

**现象**

同一路信号分别接 CH1、CH2，参数栏读数差几十 mV 到上百 mV；之前只在自检里实测过
CH1 的 GND 零点（`Frontend.h` 注释里写着 1941 / 1320），CH2 从来没标过。
而且原来的标定表只按**量程**分（S/W 两套），两个通道共用同一套参数，就算想标 CH2
也没有地方存。

**根因：标定粒度不够 + 没有设备端入口**

`Frontend.c` 的 `k_frontend_cal[2]` 下标只有量程，`Frontend_RawToMv(raw)` 也不带
通道参数；`App_Scope.c` 换算两路时调用的是同一个函数，所以 CH2 一直借用 CH1 的
零点/增益。两颗通道的偏置分压（两个 1 kΩ）和输入端 RC 都有公差，几十 mV 的差异是
正常现象，必须逐路标定。

**方案：每通道 × 每量程一套参数 + 设备端两点标定 + flash 保存**

1. `Frontend` 改为 `[通道][量程]` 四套参数：`Frontend_RawToMv(ch, raw)`，访问器全部
   带 `channel`；`Frontend_Init()` 上电从 flash 读回，缺失/校验失败用设计默认值。
2. 两点法直接测：接 GND 记 `zero_raw`，接板载 3.3 V 记 `span_raw`，由
   `(span_raw − zero_raw)` 反推比例（等价 `gain_num = 4095`、`gain_den = 码值差`），
   电阻公差、1.65 V 分压偏差、ADC 增益误差一起吸收。
3. 菜单加第 6 行 `Cal`：KEY1 切换要标定的通道（**对应用户要求的"KEY1 选哪个通道就
   标哪个通道"**），KEY4 依次采 GND / 3.3 V，KEY2 取消；标定期间暂停采集并持总线锁，
   提示画在波形区中央。
4. 结果写 **flash 扇区 11（`0x080E0000`，最后 128 KB）**，代码约 64 KB，互不影响；
   上电读回。烧写时选整片擦除会清掉标定，重新标一次即可。
5. 防错：码值差必须落在本量程的设计区间（大量程 125–270 码、小量程 950–1900 码），
   否则拒绝标定并提示 `CAL FAIL: 3V3/RANGE?`——这样"跳线乙位置与 V/div 档位错配"
   不会被固化成错误的标定值（呼应问题 23 的量程/跳线问题）。

**代价与边界**

- 新增 flash 驱动调用与标定代码：FLASH 从 55 304 B 涨到 **64 120 B / 1 MB（6.11 %）**
  （10 月 23 日修正标定换算分母后为 **64 428 B / 6.14 %**，见问题 34）；
  RAM 从 55 864 B 涨到 **56 088 B / 128 KB（42.79 %）**，CCMRAM 0。
- 擦除扇区 11 约 1–2 s，期间 CPU 取指停顿、屏幕停在 `CAL SAVING...`，属正常现象。
- 3.3 V 满点用板载 3.3 V 轨（同时是 ADC 基准），绝对精度以这条轨为准；要更准可用
  万用表量出轨电压再换算。
- 标定粒度是"当前通道 + 当前量程"：换量程（S↔W）后要分别标，因为两种量程的偏置
  增益不同。

**验证状态**

编译通过（`EXIT=0`、`-Wall` 零警告）。**未上板**，上板重点看：CH1、CH2 分别接
GND 按一次 KEY4、接 3.3 V 按一次 KEY4 后，两路读同一信号是否一致；断电重上电后
标定值是否还在（说明 flash 保存生效）；故意把跳线插错量程时是否报
`CAL FAIL: 3V3/RANGE?`；按 KEY1 切到 CH2 后 CH2 是否也被钉住（波形不再左右滚动、
触发电平虚线变灰）。

**经验**

- **标定的最小粒度是"物理通道 × 量程"**：共用一个系数只能保证"名义一致"，两路
  分压电阻和 RC 的公差会直接变成读数差。
- **两点法比"算模型 + 测零点"稳**：模型里的 1.65 V、500 Ω 都是标称值，两点直接
  消掉所有比例误差；3.3 V 轨当基准虽然不绝对准，但自洽（读自己的轨就是 3.300 V）。
- **把"防错"写进标定**：用码值差区间判断量程/跳线是否匹配，比事后查"为什么读数是
  4 倍"省事得多。

**补充（10 月 22 日）：KEY1 现在同时切触发源**

上板反馈"按 KEY1 切到 CH2 后，仍然只有 CH1 的波形被钉住"。这不是标定没生效，而是
**触发源没有跟着 KEY1 走**：触发搜索用的是 `s_st.trig_src`（默认 CH1），而 KEY1 原来
只改参数栏的测量通道（`s_st.measure_ch`），所以 CH1 被钉住、CH2 自由滚动。现在主界面
KEY1、菜单 `Cal` 行的 KEY1、标定流程里的 KEY1 都会同步 `s_st.trig_src`，做到"KEY1 选
哪一路，就测哪一路、钉哪一路"；菜单里的 `Trig src` 行仍可单独指定触发源（之后按 KEY1
会重新同步）。

### 34. 手动两点标定后，两路读数都只有实际值的一半

**现象**

按 `Cal` 流程给 CH1 / CH2 各做了一次手动两点标定（GND → 3.3 V），屏幕出现 `CAL OK SAVED`；
之后两路测同一个信号，读数都约为实际值的一半（例如输入 3.3 V 显示约 1.73 V）。

**根因**

两点标定有两个码值：接 GND 的 `zero_raw`、接 3.3 V 的 `span_raw`，两者都是**绝对码值**。
换算应该用差值：

```
输入 mV = (raw − zero_raw) × 3300 / (span_raw − zero_raw)
```

而 `Frontend_Cal_Apply()` 当时写成 `cal->gain_den = span;`，分母用了绝对码值，换算退化成
`输入 mV = codes × 3300 / span_raw`。小量程标定后零点约 1321、接 3.3 V 的码值约 2774，
比例是 `1453 / 2774 ≈ 0.524` —— 正好"一半"；大量程会变成 `195 / 2145 ≈ 9 %`（约 11 倍偏低），
只是大量程通常还没标定，所以先在小量程暴露出来。

同一处还有个语义不一致：`k_default_span_raw = {195, 1453}` 存的是**码值差**，和
"`span_raw` = 接 3.3 V 的绝对码值"的文档定义相反；默认路径只是碰巧因为分母直接用了
`span` 才"对"。

**修复（10 月 23 日）**

1. `Frontend_Cal_Apply()` 显式计算 `span_diff = span − zero`（小于 32 时钳到 32），
   `gain_den`、`min_mv`、`max_mv`、`lsb_milli_mv` 全部改用差值；`zero_mv` 不变。
2. 默认值改成绝对码值：`k_default_span_raw = {2145, 2774}`（= 零点 + 195 / 1453）。
3. `Frontend_Init()` 增加旧数据迁移：旧固件写下的条目里，**未标定**的 `span` 存的是码值差
   （195 / 1453），已标定的才是绝对码值。上电时逐条判断——绝对语义合理就直接用；否则若
   "`span` 落在本量程码值差窗口内且 `zero + span ≤ 4095`"就迁移成 `zero + span`；两者都不满足
   才整体回默认。这样已经做过的标定不会丢，重上电就按实测值换算。
4. 新增 `Frontend_Cal_DiffInWindow()` / `Frontend_Cal_EntryValid()` /
   `Frontend_Cal_EntryLegacy()` 三个判定函数，码值差窗口仍是大量程 125–270、小量程 950–1900。

**验证状态**

`EXIT=0`、`-Wall` 零警告；FLASH **64 428 B / 1 MB（6.14 %）**、RAM **56 088 B / 128 KB
（42.79 %）**、CCMRAM 0。**未上板**。上板先看：接 GND 应读 `0.00 V` 附近，接板载 3.3 V
应读约 `3.30 V`（旧标定迁移后直接生效）；若仍有偏差，按 `Cal` 流程重做一次两点标定。

**经验**

- **两点标定里"两个点"都是绝对量，只有差值参与比例换算**：存 flash、菜单显示、公式分母
  要统一语义，最好在字段名上就写清楚（`zero_raw` / `span_raw` 是码值，不是差值）。
- **默认值也要遵守同一语义**：默认表数值必须和运行时写入的格式一致，否则"没标定过的条目"
  和"标定过的条目"混在 flash 里，迁移逻辑会很难写。
- **标定后先做两个已知点自检**：GND 读 0、3.3 V 读 3.3，比拿信号源对波形更快定位
  "比例差一半 / 差一倍 / 差零点"。

---

## 十五、10 月 21 日以来的软件实现方法（FreeRTOS / 触发 / 标定）

第十一节到第十四节记的是"问题是怎么被发现、怎么定位的"；这一节按第六章的写法，
把这段时间最终落地的**软件结构、关键算法和取值**记下来 —— 不写"踩了哪个坑"，
而是写"为什么最后是这套结构"，方便以后改采集链路、加触发模式或换屏幕时照着改。

### 15.1 文件分工与调用方向

| 文件 | 职责 | 对外接口 |
|---|---|---|
| `User/App/Src/App_Rtos.c` | 三个任务的入口与循环、心跳/栈高水位统计 | `App_Rtos_Start()` |
| `User/App/Src/App_Scope.c` | 示波器状态机：采集、触发、测量、发布、按键/编码器、菜单、标定 | `App_Scope_Init/AcqStep/UiStep()` |
| `User/Algorithm/Src/Trigger.c` | 纯整数触发查找，返回亚采样位置 | `Trigger_Find()` |
| `User/Algorithm/Src/Measure.c` | 纯整数测量：Vpp / RMS / 均值 / 频率 | `Measure_Analyze()` |
| `User/Algorithm/Src/Fft.c` | 256 点整数 FFT：去直流 + Hann 窗 + 整数 dB | `Fft_Analyze()` |
| `User/App/Src/ScopeMenu.c` | 220×170 设置弹窗的绘制 | `ScopeMenu_Draw()` |
| `User/View/Src/WaveView.c` | 波形区增量刷新、触发电平标记叠加 | `WaveView_DrawTrace/Invalidate/DrawMessage()` |
| `User/BSP/Src/Frontend.c` | 逐通道标定表、raw→mV、flash 读写 | `Frontend_RawToMv/Calibrate/CalSave()` |
| `User/BSP/Src/ADC_Drv.c` | TIM2 + 双 ADC + DMA 采集驱动 | `ADC_Drv_CaptureStart/Wait/Stop()` |

调用方向是单向的：`App` 调 `Algorithm`/`View`/`BSP`；`View` 和 `Algorithm` 都不 include 任何 HAL 头。
`Trigger.c` / `Measure.c` / `Fft.c` 只依赖 `<stdint.h>`（`Fft.c` 另含同目录生成的 `FftTables.h`），
保持"可以拿到主机上单独测"的边界（本工程没有主机编译器，但结构上留了这条路）。

### 15.2 任务划分与两把互斥量

| 任务 | 优先级 | 栈 | 周期 | 干什么 |
|---|---|---|---|---|
| `App_AcqTask` | `tskIDLE_PRIORITY+4` | 1.0 KB（`configMINIMAL_STACK_SIZE*2`，最小栈 128 字） | 20 ms（下限） | 采一帧 → 换算 → 触发 → 测量 → 发布 |
| `App_UiTask` | `+3` | 2.5 KB（`*5`） | 10 ms | 读编码器/按键 → 处理 → 拉帧 → 重绘（取锁最多等 20 ms） |
| `App_MonitorTask` | `+1` | 0.5 KB（`*1`） | 500 ms | 心跳、剩余堆、两个任务栈高水位 |

两个循环的关键写法：

```c
/* acq：采完一帧显式让出 1 tick */
if (App_Scope_AcqStep() != 0U) {
    vTaskDelay(1U);
}
vTaskDelayUntil(&last, pdMS_TO_TICKS(APP_ACQ_PERIOD_MS));   /* 20 ms */

/* ui：10 ms 周期用不满，也让 1 tick 给 mon/idle */
drew = App_Scope_UiStep();
if (drew != 0U) {
    g_monUiFrames++;
}
vTaskDelay(1U);
vTaskDelayUntil(&last, pdMS_TO_TICKS(APP_UI_PERIOD_MS));     /* 10 ms */
```

两把互斥量分工不同：

| 锁 | 保护什么 | 谁拿 | 持有多久 |
|---|---|---|---|
| `s_lock` | `s_frame` 与 `s_view` 之间的 2×1280 B 拷贝 + `seq` | acq 发布 / ui 拉取 | 微秒级 |
| `s_bus_lock` | "ADC DMA 突发"与"LCD SPI 突发"不同时发生（问题 28 的噪声修复） | acq 采集段 / ui 绘制段 | 40 kSa/s 约 16 ms + 2 ms；慢时基可到几百 ms |

**为什么 acq 每帧要 `vTaskDelay(1)`**：`xSemaphoreGive()` 只是把等待者从事件列表移到就绪列表，
并不把所有权交给它。acq 优先级比 ui 高，give 之后如果它继续跑，下一轮 `App_BusLock()` 会立刻把
计数=1 的锁重新拿走，ui 醒来再取就是 0 —— 这就是"按 KEY4 后按键/编码器十几秒才响应一次"的成因
（问题 32）。让出 1 tick 后 ui 必定能在这一拍拿到锁画完；代价是每帧多 1 ms，40 kSa/s 下约 5 %。

**为什么 ui 取锁要有上限**：慢时基一帧 640 ms，ui 不能死等，否则按键扫描也被拖住。
`App_BusLockWait(APP_UI_BUS_WAIT_MS = 20)` 拿不到就返回 0，本帧不画，`s_draw_pending` 留着下一轮再试。

**经验**
- 互斥量的 give 不是 handoff：把它当"谁先醒谁拿"的计数器，不要当"交钥匙"。
- 优先级只决定"就绪后谁先跑"，不决定"谁先拿锁"。
- "要不要画"和"画成没有"要分开：`s_draw_pending` 只由 ui 任务读写，不需要再加锁。

### 15.3 一帧采集的完整链路

```
TIM2 TRGO（sps）→ ADC1/ADC2 同步转换 → DMA2_Stream0 搬 640×32bit
      ↓
ADC_Drv_CaptureWait（NDTR==0 判完成）→ CaptureStop
      ↓
s_raw[640] → s_mv1[640] / s_mv2[640]（前端零点/增益，两路各一套标定）
      ↓
Trigger_Find（选中的那一路上找）→ index + frac_q16
      ↓
App_ResampleWindow（两路用同一 index/frac，就地插值成 320 点）
      ↓
Measure_Analyze（两路各 320 点）→ App_PublishFrame（seq++）
      ↓
App_PullFrame（ui）→ WaveView_DrawTrace（增量刷新）
```

核心几行：

```c
ADC_Drv_CaptureStart(s_raw, APP_CAP_RAW);                  /* 640 点，正常模式 DMA */
if (ADC_Drv_CaptureWait(App_CaptureTimeoutMs(sps)) == ADC_DRV_OK) { /* ... */ }
ADC_Drv_CaptureStop();
for (i = 0U; i < APP_CAP_RAW; i++) {
    s_mv1[i] = Frontend_RawToMv(FRONTEND_CH1, ADC_Drv_WordCh1(s_raw[i]));
    s_mv2[i] = Frontend_RawToMv(FRONTEND_CH2, ADC_Drv_WordCh2(s_raw[i]));
}
```

超时公式：`ms = (640 × 1000) / sps + 100`，上限 1500。

| sps（时基） | 一帧长度 640/sps | `App_CaptureTimeoutMs` |
|---|---|---|
| 40 kSa/s（1 ms/div，默认） | 16 ms | 116 ms |
| 200 kSa/s（200 µs/div） | 3.2 ms | 103 ms |
| 1 kSa/s（50 ms/div 及以上） | 640 ms | 740 ms |

`win_start` 决定发布哪一段 320 点：

| 触发状态 | `win_start` | 含义 |
|---|---|---|
| `LOCKED`（找到触发点） | 0 | 重采样后的前 320 点，触发点在下标 160（屏幕中线） |
| `MISSING`（没找到） | 160 | 原始缓存的中间 320 点，自由滚动 |
| `OFF`（触发关闭） | 160 | 同上，不搜索、不画标记 |

缓冲：`s_raw[640]`、`s_mv1[640]`、`s_mv2[640]` = 3 × 2560 B = 7.5 KB；从 320 点改到 640 点时
RAM 正好多 3.75 KB。超时用的是 `APP_CAP_RAW`（640）而不是 320，否则慢时基下会"没采完就判超时"。

**经验**
- 一帧的长度由时基决定，不由任务周期决定：640/sps，40 kSa/s 是 16 ms，1 kSa/s 是 640 ms。
  `vTaskDelayUntil` 的 20 ms 只是"最快多久一轮"。
- 采集段整段持总线锁（Start → Wait → Stop → 换算 → 触发 → 插值），保证 DMA 突发与 LCD 突发不重叠；
  `Measure_Analyze` 与发布在锁外做（此时缓存不会再被别人改）。
- `seq` 同时是帧号和新帧标志，ui 只比较它，不为"有没有新帧"再设一个变量。

### 15.4 触发搜索与亚采样对齐

触发模块的接口只有四个类型/函数：

```c
typedef enum {
    TRIGGER_EDGE_OFF = 0,
    TRIGGER_EDGE_RISING = 1,
    TRIGGER_EDGE_FALLING = 2
} Trigger_Edge;

typedef struct {
    int32_t      level_mv;       /* 触发电平（mV，绝对值） */
    int32_t      hysteresis_mv;  /* 迟滞半宽（mV） */
    Trigger_Edge edge;
    uint16_t     pre_samples;    /* 触发点前保留多少点 */
    uint16_t     post_samples;   /* 触发点后保留多少点 */
} Trigger_Config;

typedef struct {
    int32_t  index;     /* 穿越点前一刻的采样下标；-1 = 没找到 */
    uint16_t frac_q16;  /* 穿越点在 index..index+1 之间的 Q16 位置 */
} Trigger_Point;

Trigger_Point Trigger_Find(const int32_t *mv, uint16_t count,
                           const Trigger_Config *cfg);
```

**搜索区间**：`count = 640`、`pre = post = 160`，所以 `k ∈ [160, 479]`。
`k` 是"穿越前一刻"，保证 `k ≥ 1`、`k+1 < count`，且显示窗口 `start = k − 160` 落在 `[0, 319]`。

**判定条件**（迟滞带 `h`，上升沿）：

```
mv[k] <= level − h  且  mv[k+1] >= level + h
```

下降沿把不等号反过来。`h = 触发通道 V/div ÷ 20`，最小 1 mV：

| V/div | 迟滞 h |
|---|---|
| 100 mV/div | 5 mV |
| 500 mV/div | 25 mV |
| 1 V/div | 50 mV |
| 10 V/div | 500 mV |

迟滞不是"滤波"，是**防重复触发**：信号在电平附近抖动时，若只判"穿过电平"，噪声会造出很多个
候选穿越点，触发点在其中跳来跳去，屏幕反而更抖。加上"必须先从电平下方 h 以外进到上方 h 以外"
之后，噪声不会造出额外的命中；`Trigger_Find` 取**第一个**命中的 `k`（周期信号就是显示窗里的
第一个上升沿），后面的命中不再看。

**亚采样位置**：只对齐到采样网格（`k`）时，真实穿越点落在 `k..k+1` 之间的随机位置，
每帧差最多 1 个采样点；在屏幕上就是 ±1 像素的横向抖动。行指纹（15.5）只要有一列变了就整行失效，
于是每帧退化成整屏重画（问题 30）。所以返回 Q16 的分数位置：

```
frac = (level − v0) × 65536 / (v1 − v0)      // clamp 到 [0, 65535]
```

上层把显示窗起点 `a = k − 160 + i` 处的两个相邻采样线性插值到"真实穿越时刻"：

```c
v0 = buf[a];
v1 = buf[a + 1];
buf[i] = v0 + (int32_t)(((int64_t)(v1 - v0) * frac_q16) / 65536);
```

`i = 160` 时输出值正好等于 `level` —— 触发点永远钉在屏幕中线。两路用**同一个** `index/frac`
重采样（同一时基、同一时刻），所以 CH1 触发时 CH2 也是对齐的。

**没找到怎么办**：先按"信号中点"再试一次 —— `App_AutoLevel()` 扫 640 点取 `(min+max)/2`，
与当前 `level_mv` 不同就改电平重试；命中则把新电平写回 `s_st.trig_level_mv`（菜单里能看到它变了）。
这就是"单极性 PWM 默认 0 V 触发电平怎么都钉不住"的解法（问题 31）。纯直流信号没有穿越点，
AUTO 也救不了，此时按没触发处理。

| 状态 | 触发源 | 标记 | 显示 |
|---|---|---|---|
| `APP_TRIG_STATE_OFF` | 不搜索 | 不画 | 自由滚动，`win_start = 160` |
| `APP_TRIG_STATE_LOCKED` | 命中 | 灰虚线 | 钉住，`win_start = 0` |
| `APP_TRIG_STATE_MISSING` | 未命中 | 红虚线 | 自由滚动，`win_start = 160` |

**经验**
- 触发不改变采样率，只改变"从哪一点开始显示"；窗口始终是 320 点。
- 返回 `index`（前一拍）而不是 `index+1`，是为了让"窗口起点 = index − pre"永远合法。
- 两路共用一个触发点：要看另一路触发就切 `Trig src`（或按 KEY1，见 15.8），不要做两套独立触发。

### 15.5 触发电平标记与增量刷新

`WaveView_Data` 里加了两个字段：

```c
int32_t trig_mv;      /* 触发电平（mV） */
uint8_t trig_state;   /* 0=关 1=锁定 2=未触发 */
```

标记的 y 和波形用同一个映射：

```
y = VIEW_WAVE_MID_Y − trig_mv × VIEW_PX_PER_VDIV / vdiv_mv
  = 117 − trig_mv × 19 / vdiv_mv          // 钳到 [22, 211]
```

叠加层 `Wave_OverlayTrigger()` 在行合成之后画：从 `x = 2` 开始"亮 2 灭 2"的虚线，宽度铺满 320 点，
再在 `x = 1..5` 画一个小实心方块（左端锚点）。颜色：

| 状态 | 颜色 | 实际观感 |
|---|---|---|
| 锁定（1） | `VIEW_COLOR_LABEL`（0x8410 灰） | 灰 |
| 未触发（2） | `VIEW_COLOR_SELECT`（0xF800 红） | 偏青/偏蓝（面板 R/B 反序，见 6.8） |
| 关闭（0） | 不画 | — |

标记是"叠加层"不是数据：`Wave_FlushRows()` 先合成行模板，再叠 CH1、CH2，最后如果这一行
`y == trig_y` 才叠标记；哈希比较在最后，所以标记不变时这一行仍然跳过 SPI。

增量刷新的关键在"标脏哪几行"：`WaveView_DrawTrace()` 每帧把**上一帧的标记行**（若上一帧状态≠0）
和**这一帧的标记行**（若本帧状态≠0）都标脏，然后只重画脏行。标记从"灰"变"红"、或电平被 AUTO
改写时，旧行会被恢复成纯波形/网格，新行画上新标记 —— 不需要整屏重画。关掉触发时状态=0，
旧行标脏后自然把标记擦掉。

**注意**：菜单弹窗（15.6）覆盖 y = 32..201，正好盖住中线 y = 117，所以**在菜单里调电平时看不到
那条虚线**，只能看 `Trig level` 的数值；按 KEY2 退出菜单时 `WaveView_Invalidate()` 会整片重画，
线就出现了。这是覆盖式弹窗的固有限制，不是标记没画。

**经验**
- 标记画在"行合成"阶段，不写进 `s_mv*` 数据，所以它不影响测量、也不进帧缓存。
- 增量刷新下，任何"每帧都可能变"的叠加层都必须参与标脏和哈希，否则会留下残影。
- 颜色按"配置值"写文档会误导：这个面板的红实际发青，认线时以实际观感为准（6.8）。

### 15.6 设置菜单

弹窗是覆盖式绘制（不保存底下内容），几何参数集中在 `ScopeMenu.c` 顶部：

| 常量 | 值 | 说明 |
|---|---|---|
| `MENU_W` / `MENU_H` | 220 / 170 | 7 行，比 6 行版高 20 |
| `MENU_X` | `(320−220)/2 = 50` | 水平居中 |
| `MENU_Y` | 32 | 波形区 22..211 内垂直居中：`22 + (190−170)/2` |
| `MENU_ROW_Y(i)` | `MENU_Y + 24 + i×20` | i=0..6 → 56 / 76 / 96 / 116 / 136 / 156 / 176 |
| 标题 | `MENU_Y + 4` | "SETTINGS"，红字 |
| 底部提示 | `MENU_Y + MENU_H − 字号 − 4` | "KEY2: back   ROT: change"，灰字 |

七行的取值来源与编码器2 的行为：

| 行 | 名称 | 值 | 编码器2 改什么 |
|---|---|---|---|
| 0 | `V/div C%u` | 100 mV … 10 V（7 档，**按当前通道**，行内并列显示两路值如 `500m/2.0V`） | 改当前通道档位 + `Frontend_SetRange(ch)`（≤1 V 档 = S，≥2 V 档 = W） |
| 1 | `Time/div` | 20 µs … 100 ms（12 档） | 换档 + 重算 `sps`（`40 000 000 / tdiv_us`，钳 1 k..1 M） |
| 2 | `Trig src` | CH1 / CH2 | 只切触发源（并夹触发电平 ±5 格） |
| 3 | `Trig edge` | OFF / RISE / FALL | mod 3 循环 |
| 4 | `Trig level` | 带符号 mV | 步进 `触发通道 V/div ÷ 10`，钳 ±5 格 |
| 5 | `Auto set` | 显示 `KEY4` | 该行特殊：KEY4 执行一次自动设置，编码器2 不改值 |
| 6 | `Cal` | CH1 / CH2 | 该行特殊：KEY1 切通道、KEY4 开始标定，编码器2 不改值 |

按键映射（菜单打开时）：

| 输入 | 行为 |
|---|---|
| 编码器1 | 在 7 行之间上下移动（循环） |
| 编码器2 | 改当前行的值（`Auto set` / `Cal` 行除外） |
| KEY2 | 退出菜单：`menu = 0`、`WaveView_Invalidate()`，下一帧整片重画 |
| KEY1 | **任何一行**都切当前通道（测量 + 触发源 + 编码器1 的 V/div + 标定目标） |
| KEY4 | `Auto set` 行执行自动设置；`Cal` 行启动标定 |
| KEY3 | 菜单里不响应 |

每行绘制顺序：整行底色（选中 `VIEW_COLOR_SELECT` 红底，否则 `VIEW_COLOR_BAR_BG` 黑底）→
左标签（左对齐）→ 右侧数值（用 `Lcd_TextWidth()` 右对齐）。数值格式化集中成四个函数：

| 函数 | 例 |
|---|---|
| `Menu_FormatVdiv` | `500mV` / `1.00V` |
| `Menu_FormatTdiv` | `1.00ms` / `20us` |
| `Menu_FormatEdge` | `OFF` / `RISE` / `FALL` |
| `Menu_FormatLevel` | `+500mV` / `-1.20V` |

**经验**
- 覆盖式弹窗最简单：不用保存背景，但退出时必须整片重画（`WaveView_Invalidate()`），否则波形区会
  残留弹窗边角。
- 菜单里只响应 KEY2 和两个编码器；KEY3 不响应，KEY4 仅在 `Auto set` / `Cal` 行有定义，
  避免"想调电平却把运行状态切了"。
- 单位换算只写在格式化函数里：10 月 23 日加 `Auto set` 行时只新增一个 switch case 和一行动作，
  绘制逻辑没动。

### 15.7 逐通道两点标定

**数据模型**：`(通道, 量程)` 四套参数，每套只存两个原始码值 —— 零点 `zero_raw`（接 GND 采到）
和满点 `span_raw`（接 3.3 V 采到的**绝对码值**，不是差值）。换算只用两者之差：

```
输入 mV = (raw − zero_raw) × 3300 / (span_raw − zero_raw)
```

| 派生量 | 公式 | WIDE 例（zero=1950, span=2145） |
|---|---|---|
| 码值差 `span_diff` | `span − zero` | 195 |
| `gain_num` / `gain_den` | 4095 / `span_diff` | 4095 / 195 |
| `zero_mv` | `(zero×3300 + 2047) / 4095` | 1571 mV |
| `min_mv` | `−zero×3300 / span_diff` | −33 000 mV |
| `max_mv` | `(4095−zero)×3300 / span_diff` | +36 300 mV |
| `lsb_milli_mv` | `3300×1000 / span_diff` | 16 923（即 16.923 mV/LSB） |

上电默认值（两通道相同，标定后各自独立）：

| 量程 | `zero_raw` | `span_raw`（绝对码值） | 码值差 |
|---|---|---|---|
| SMALL（S，前 4 档 V/div） | 1321 | 2774 | 1453 |
| WIDE（W，后 3 档 V/div） | 1950 | 2145 | 195 |

**flash 记录**（28 B = 7 个 32 位字，放扇区 11 起始 `0x080E0000`）：

| 偏移 | 大小 | 字段 | 值 |
|---|---|---|---|
| 0x00 | 4 | `magic` | `0x314C4143`（"CAL1"） |
| 0x04 | 4 | `version` | 1 |
| 0x08 | 8 | `zero_raw[2][2]` | [通道][量程] |
| 0x10 | 8 | `span_raw[2][2]` | [通道][量程]，接 3.3 V 的绝对码值 |
| 0x18 | 4 | `checksum` | 前 6 个字之和 |

**防错**：写 RAM 之前先校验"码值差"，落在设计窗口才接受：

| 量程 | 合法码值差 | 说明 |
|---|---|---|
| WIDE | 125 – 270 | 3.3 V 输入应产生约 195 码 |
| SMALL | 950 – 1900 | 应产生约 1453 码 |

窗口不匹配说明"跳线位置与 V/div 档位错配"（例如软件以为在 W、硬件还在 S），此时拒绝标定并提示
`CAL FAIL: 3V3/RANGE?`，避免把错误的比例固化进 flash（呼应问题 23）。

**保存步骤**（`Frontend_CalSave()`）：`HAL_FLASH_Unlock()` → 清 EOP/OPERR/WRPERR/PGAERR/PGPERR/PGSERR
→ `HAL_FLASHEx_Erase()`（`FLASH_SECTOR_11`、BANK_1、`VOLTAGE_RANGE_3`、1 个扇区）→ 逐字
`HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD)` × 7 → `HAL_FLASH_Lock()`。擦除 128 KB 扇区约 1–2 s，
期间取指停顿、屏幕停在 `CAL SAVING...`，属正常。

**上电读回**（`Frontend_Init()`）：`magic`、`version`、`checksum` 全对后逐条校验四组 `(zero, span)`：
按"绝对码值"语义合法（`span−zero` 落在本量程窗口）就直接用；否则若是旧固件写下的旧格式
（`span` 存的是码值差，要求 `zero+span ≤ 4095` 且落在窗口内）就迁移成 `zero+span`；两者都不满足
则整组回落到设计默认值 —— 这样"整片擦除把标定擦了"不会让设备读到随机数据，**旧标定也不会丢**
（问题 34）。

**设备流程**（`Cal` 行按 KEY4 进入）：

| 状态 | 界面提示 | KEY4 | 其它键 |
|---|---|---|---|
| `APP_CAL_GND` | `CAL CHx/S GND KEY4  K1:CH K2:ESC` | 采 64 次取平均存 `s_cal_zero_raw` → SPAN | KEY1 换通道并回 GND；KEY2 取消 |
| `APP_CAL_SPAN` | `CAL CHx/S 3V3 KEY4  K1:CH K2:ESC` | 采 64 次平均得绝对码值 `raw`，`Frontend_Calibrate(zero, raw)` 校验差值窗口并写 RAM/flash | 同上 |
| `APP_CAL_DONE` | `CAL OK  SAVED` / `CAL OK  SAVE FAIL` | 立即退出 | 1.5 s 后自动退出，KEY2 也退出 |

失败时不会进 `DONE`：采样失败提示 `CAL FAIL: ADC BUSY  KEY4`，码值差不在窗口提示
`CAL FAIL: 3V3/RANGE? KEY4`，都停在当前步骤等重试（KEY2 取消）。

标定期间 `running = 0`（暂停采集），每次采样都持总线锁（64 次 `ADC_Drv_ReadOnce`，几十 µs×64），
结束后恢复原来的 RUN/HOLD 状态并回到菜单。`Frontend_Calibrate()` 成功即已写入 RAM 并生效，
`Frontend_CalSave()` 只负责掉电保存 —— 所以看到 `CAL OK  SAVE FAIL` 时，本次标定仍可用，只是重启会丢。

**经验**
- **flash/内存里存两个绝对码值，换算只用差值**：`zero_raw` = GND 码值、`span_raw` = 3.3 V 码值，
  分母是 `span−zero`；两者语义不能混（混了就是问题 34 的"读数一半"）。
- 码值差就是增益的倒数：不需要知道 1.65 V、500 Ω 这些标称值，两点直接消掉所有比例误差。
- 防错窗口比精度更重要：量程/跳线错配时读数会差 4–5 倍（问题 23），窗口校验能在标定这一步拦住。
- 先写 RAM 再写 flash：flash 失败不影响本次使用，提示里也把"已生效"和"已保存"分开说。

### 15.8 KEY1 的"当前通道"语义（10 月 22 日）

KEY1 现在同时改三个消费者：

| 消费者 | 字段 | 作用 |
|---|---|---|
| 参数栏测量 | `s_st.measure_ch` | 底部 F / T / Vpp / RMS / DC 测哪一路 |
| 触发源 | `s_st.trig_src` | 哪一路被"钉住"（`App_Capture` 里二选一） |
| 标定目标 | `s_cal_ch` / `s_st.measure_ch` | `Cal` 行要标哪一路 |

三处入口都要同步，少一处就会出现"选中的没钉住、没选中的反而钉住"：

```c
/* App_HandleMain 的 KEY1、App_HandleMenu 的 Cal 行 KEY1 */
s_st.measure_ch = (s_st.measure_ch == 1U) ? 2U : 1U;
s_st.trig_src   = s_st.measure_ch;

/* App_CalStart / App_CalHandle 的 KEY1 */
s_st.trig_src   = s_cal_ch;
```

菜单里的 `Trig src` 行仍可单独指定触发源（例如"用 CH1 触发、看 CH2 参数"），之后按 KEY1 会重新同步。

**经验**：用户说的"当前通道"是一个整体概念，不是某一个字段；凡是"跟着 KEY1 走"的东西，
列一张消费者表逐个改，比事后按现象修更稳。

### 15.9 资源占用与验证状态

| 项目 | 当前值 | 占比 |
|---|---|---|
| FLASH | 70 088 B / 1 MB | 6.68 % |
| RAM | 59 688 B / 128 KB | 45.54 % |
| CCMRAM | 0 B | — |

各阶段的增量（用于以后判断"哪次改动贵"）：

| 阶段 | FLASH | RAM |
|---|---|---|
| 触发功能之前 | 52 532 B | 52 008 B |
| + 640 点采集与触发查找 | 54 228 B | 55 864 B |
| + 亚采样插值 | 54 844 B | 55 864 B |
| + AUTO 电平 | 55 048 B | 55 864 B |
| + 调度修复（让 1 tick / 有界取锁） | 55 304 B | 55 864 B |
| + 逐通道标定（含 HAL flash 驱动） | 64 088 B | 56 088 B |
| + KEY1 同步触发源 | 64 120 B | 56 088 B |
| + 标定分母修复（`span−zero`） | 64 428 B | 56 088 B |
| + AUTO 自动设置（V/div、Time/div） | 65 380 B | 56 088 B |
| + FFT 频谱（256 点整数 FFT + 频谱显示） | 69 660 B | 59 680 B |
| + 分通道 V/div（逐通道量程、状态栏两值两字母） | 70 088 B | 59 688 B |

**帧率预期**：40 kSa/s、稳定触发时，行指纹大部分命中，一帧只刷变化的几十行，实测约 26–43 fps；
没触发/自由滚动时每帧整片重画（46–122 KB、50–110 ms），看起来会慢 —— 这不是"又退化了"，而是
触发确实没找到点（红线）。1 kSa/s 时一帧本身就要 640 ms，按键响应靠 ui 的有界取锁保持可用。

**验证状态**：以上全部只经过 ARM 构建（`-Wall` 零警告）+ 静态审查，**未上板实测**。上板重点：

1. 1 kHz 正弦、默认 500 mV/div：波形应钉住，电平线为灰色；
2. 把 `Trig level` 调到信号范围外：线变红/青、波形自由滚动，AUTO 不应死循环；
3. `KEY1` 切 CH2：CH2 应被钉住（线仍是灰的），参数栏同时切到 CH2；
4. 两路分别接 GND / 3.3 V 各按一次 KEY4：看到 `CAL OK SAVED`，之后两路读同一信号应一致；
5. 断电重上电：flash 标定值应还在；
6. 50 ms/div（1 kSa/s）：按键/编码器仍应即时响应，只是画面约 0.6 s 一帧。
7. 分通道 V/div：两路设不同档位，状态栏两个 V/div 值与两个量程字母应各自对应；
   KEY1 切通道后编码器1 只改当前那一路；菜单 `V/div C1` 行同时显示两路值（`500m/2.0V`）。
8. 菜单 `Auto set` 行按 KEY4：小信号应自动往灵敏档调、大信号往粗档调，1~2 帧后数值收敛；
   单极性 0…3.3 V 信号不应再削顶。
9. `KEY3` 切到 FFT：1 kHz 正弦应在第 0 格与第 1 格之间出现一根谱线（40 kSa/s 时每格 2.50 kHz）；
   状态栏 V/div 位置显示 `FFT`、Time 位置显示 `2.50kHz/div`；HOLD 后频谱冻结；
   直流输入（探头短路到 GND）应基本没有谱线，而不是在 0 Hz 处顶出一根。

---

### 15.10 AUTO 自动设置（10 月 23 日）

**需求**：需求文档 §7 的 AUTO 只剩"自动选择 V/div 与 Time/div"（触发电平的 AUTO 见第十二节）。
10 月 23 日按下面的规则落地：入口在设置菜单第 6 行 `Auto set`，按 KEY4 执行一次。

**选档规则**（`App_AutoVdiv()` / `App_AutoTdiv()`，都在 `App_Scope.c`）：

| 项目 | 规则 | 理由 |
|---|---|---|
| V/div | 对**实际显示的通道**求 320 点显示窗的 min/max：`Vpp = max − min`、`peak = max(\|min\|, \|max\|)`。从最灵敏档开始找第一档同时满足 `Vpp ≤ 6 格` 且 `peak ≤ 4.5 格`；都不满足用最粗档 | 6 格留 1 格余量给噪声/漂移；`peak ≤ 4.5 格` 保证双极性波形不越过满屏 ±5 格，单极性大信号（0…3.3 V）也会被推到合适档位 |
| Time/div | 周期取 `meas[measure_ch−1].period_us`（无效时试另一路）；选最小的 `k_tdiv_us[i]` 使 `8 × tdiv ≥ 2 × period`（屏幕 8 格至少放下 2 个周期）；周期无效则保持当前时基 | 1-2-5 步进下"2 个周期"条件实际落到 2~5 个周期，既看得清形状又不至于挤成一条线 |

**多帧收敛**（关键）：按下 KEY4 时只用屏幕上已有的那一帧（`s_view.mv1/mv2`）判一次，
然后置 `s_auto_pass = 2`：`App_Scope_UiStep()` 每拉到一帧新数据就再执行一次 `App_AutoSetup()`，
最多 2 次。原因是按下瞬间可能正处在"你想纠正的错误状态"里：

- 量程太灵敏 → 波形削顶，测得的 Vpp 偏小 → 第一遍可能选得太灵敏 → 用新帧再判把档位推上去；
- 时基太慢 → 一帧 320 点装不下几个周期，`period_us` 测不准 → 新帧（新时基）再判一次周期。

**与触发的关系**：AUTO 只改 V/div、Time/div（以及把触发电平夹回 ±5 格），触发源/边沿不动；
调完 V/div 后若电平落到信号范围外，采集侧的 AUTO 电平会在下一帧自动取中点写回 `Trig level`。

**实现细节**：

- 菜单行：`SCOPE_MENU_ROWS 7`、`ROW_AUTO 5`（`Cal` 顺延为 6），弹窗 **220×170 @ (50,32)**，
  行基线 y = 32+24+i×20 = 56 / 76 / 96 / 116 / 136 / 156 / 176。
- `App_HandleMenu()` 顶部：任何按键/编码器动作都清 `s_auto_pass = 0`，防止离开菜单后还在改档位。
- `Auto set` 行按 KEY4：`s_view.seq != 0` 时调用 `App_AutoSetup(s_view.mv1, s_view.mv2, s_view.meas)`，
  `s_auto_pass = 2`，`return 1` 让本帧重画菜单数值。
- UI 步进函数在标定分支之后、正常绘制之前检查 `s_auto_pass`：`App_PullFrame()` 拉到新帧才递减并重算。

**代价与边界**：

- 只增加 FLASH 952 B（64 428 → **65 380 B / 6.24 %**；之后加入 FFT 后为 69 660 B / 6.64 %），
  RAM 不变（复用现有缓冲，CCMRAM 0）。
- 只看幅度/周期，不看占空比、不看噪声；极小信号（< 60 mV）测不到频率，Time/div 保持不动。
- HOLD 状态下只有第一遍（没有新帧可判），想收敛先 RUN 再按。
- 未上板验证。上板重点：小信号/大信号各按一次应各归其位；单极性 0…3.3 V 不应削顶；
  换档后 1~2 帧内数值收敛；AUTO 后触发仍能锁定。

**经验**：自动量程不是"算一次就完"——输入数据本身可能正处在你想纠正的错误状态里，
所以要"改 → 等新数据 → 再判"的闭环；把待执行次数做成一个 UI 私有计数器（`s_auto_pass`），
比在采集任务里加逻辑简单，也不会破坏"采集与绘制互斥"的既有约束。

---

### 15.11 FFT 频谱（10 月 23 日）

**需求**：需求文档 §7 的最后一项软件功能——FFT 频谱。要求不破坏现有交互与刷新率，
继续走"整数定点、不链接 libm"的路线。

**显示层：复用 WaveView，而不是另写一套频谱绘制**

新写一个 `SpectrumView` 意味着把"行模板 / 脏行 / 行指纹 / SPI 批量写"整套重来一遍，
切换显示模式时还容易留残影。这里换了个思路：把 0 … −60 dB 的 128 条谱线先合成为
一条 **320 点"假波形"**，再交给现有的 `WaveView_DrawTrace()`：

```
 dB(0..-60) → 高度 h(0..189) → y = 211-h → 假 mV = (117-y)*100
```

取 `vdiv = 1900`（`APP_FFT_VDIV`）时 `Wave_SampleToY()` 正好反推出 `y = 117 − mv/100`，
于是网格、脏行、FNV-1a 行指纹、触发电平线的隐藏全部沿用现成逻辑；频谱稳定时行指纹
照样命中，帧率与时域波形同一量级。切换显示模式也不用整屏重画——`prev/cur y` 数组
会自然把旧谱线标脏。

**算法：256 点整数 FFT（`Fft.c`）**

- 取显示窗（320 点）居中 256 点（下标 32 … 287，触发点 160 在正中）。
- **先减均值去直流**，再乘 Hann 窗（Q15）；不先减均值的话，窗函数会把直流泄漏到
  bin1/bin2，纯直流输入会显示成一条低频假谱线。
- radix-2 DIT，旋转因子 Q15 查表；蝶形乘法用 int64 中间量（|b| ≤ 4.6e6，×32767 ≈ 1.5e11）。
- 功率 `P = re² + im²` 用 int64；`dB = 10·log10(P/Pmax)` 用整数 log2 实现：
  `log2 Q8 = 指数 + 256 项尾数表`，再乘 3.0103（Q8 = 771）/ 65536，钳到 [−60, 0]。
- bin0（直流）固定 −60、不参与最强谱线选择；`peak_bin` 输出最强交流谱线下标。
- 表由 `tools/gen_fft_tables.py` 生成（`FftTables.h`：Hann 256、cos/sin 128 + 128、log2 256）。

**显示映射与状态栏**

| 项目 | 规则 |
|---|---|
| 横轴 | 8 格 = 0 … 采样率/2，每格 = 采样率/16；谱线间隔 = 采样率/256，每像素 = 采样率/640 |
| 纵轴 | 0 … −60 dB，相对最强谱线；线性铺满 190 px（60 dB 占 189 px） |
| 进入方式 | KEY3 第 4 态；频谱源 = 当前通道（KEY1），颜色沿用该通道曲线色 |
| 状态栏 | `StatusBar_Info.fft = 1` 时：V/div 位置显示 `FFT`、Time 位置显示"频率/格" |
| 参数栏 | 不变，仍是当前通道的时域测量值（F/T/Vpp/RMS/DC） |
| 触发线 | 频谱模式下 `trig_state = OFF`，不画触发电平虚线 |

**代价与边界**

- FLASH 65 380 → **69 660 B / 6.64 %**（+4 280 B：表约 1 KB + FFT/显示/状态栏代码 3.2 KB）；
  RAM 56 088 → **59 680 B / 45.53 %**（+3 592 B：`s_re/s_im` 2 KB + 假波形 `s_spec` 1.25 KB
  + 结果缓冲约 0.26 KB），CCMRAM 0。
- 频谱固定 256 点，与 Time/div 无关：分辨率 = 采样率/256；想看低频细节就把时基调快
  （提高采样率会同时抬高频率分辨率数值，需要按需取舍）。
- 只显示幅度，不显示相位；纵轴相对最强谱线，看不到绝对幅度（绝对幅度看时域 Vpp/RMS）。
- 单次 FFT 在 UI 任务里算（约 1~2 ms @ -O0），只在出现新帧时重算；HOLD 时冻结。

**验证状态**

- 没有主机 C 编译器，用一份与 C 逐行对应的 Python 等价实现做了自检（临时脚本，已删）：
  1 kHz / 40 kSa/s 的峰值落在 bin 6（期望 6.4）；整数 dB 与浮点参考最大差 0.85 dB；
  直流输入全部压到 −60 dB；dB → 像素 → 假 mV 往返一致；噪声输入不越界。
- ARM 构建 `EXIT=0`、`-Wall` 零警告。
- **未上板**。上板重点：KEY3 切 FFT 后有信号时谱线位置与"频率/格"对得上；探头接 GND 时
  不应出现假低频线；HOLD 后频谱冻结；频谱模式刷新率应接近时域模式（稳定时只刷少量行）。

**经验**

- 增量刷新的关键是"脏行"而不是"画什么"：把新视图伪装成一条现有曲线，比新写一套
  绘制便宜得多，也自动继承了行指纹跳过。
- 窗函数前必须去直流，否则直流泄漏会被误读成低频信号——这是 FFT 示波器最常见的假谱线来源。
- 整数 dB 用 `log2(功率)` 的 Q8 查表即可，不需要 sqrt + log10 浮点，误差 < 1 dB，够显示用。

---

### 15.12 分通道 V/div（10 月 23 日）

**需求**：PROJECT_SPEC §7 的"分通道 V/div"——两路各自选垂直档位。10 月 20 日曾为了
"同屏两路用同一套电压-像素换算"把 V/div 改成双通道共用（问题 18），这次又改回独立：
用"状态栏把两个值都显示出来"消除"看起来一样却读数不同"的困惑，而不是限制档位。

**数据模型**

- `App_State.vdiv_idx[2]`：`[0]` = CH1、`[1]` = CH2（旧版是单个 `uint8_t`）。
- `App_VdivMv(channel)` 返回该通道的 `k_vdiv_mv[vdiv_idx[...]]`；
  `App_TrigVdivMv()` = `App_VdivMv(s_st.trig_src)`（触发相关计算统一用它）。
- `App_ApplyRanges()` 按各通道自己的 `vdiv_idx` 分别 `Frontend_SetRange(CH1/CH2, SMALL/WIDE)`；
  旧 `App_ApplyRange()` 已删除。

**Frontend 逐通道量程**

- API 全部带通道：`Frontend_SetRange(uint8_t channel, Frontend_Range range)`、
  `Frontend_GetRange(uint8_t channel)`、`Frontend_RangeLetter/RangeName/JumperHint(uint8_t channel)`。
- `static Frontend_Range s_range[2]`；`Frontend_Range_Index(channel)` 取该通道量程；
  `Frontend_Cal_Of(channel)` 用该通道量程选标定表；`Frontend_Calibrate()` 用
  `Frontend_Range_Index(channel)` 判断码值差窗口（WIDE 125–270 / SMALL 950–1900）。
- `Frontend_Init()` 两路都置 `FRONTEND_RANGE_SMALL`；标定仍是每通道每量程 4 套。

**"触发通道"的消费者**

触发搜索、迟滞、电平范围/步进都跟"触发源那一路"的 V/div 走，而不是 CH1：

| 位置 | 计算 |
|---|---|
| `App_TriggerConfig()` 迟滞 | `App_TrigVdivMv() / 20`（至少 1 mV） |
| `App_ClampLevel()` 限幅 | `±5 × App_TrigVdivMv()` |
| 菜单 `Trig level` 步进 | `App_TrigVdivMv() / 10` |
| `WaveView_Data.vdiv_mv_trig` | 触发电平虚线的 y 用触发通道的 V/div 映射（`Wave_SampleToY(trig_mv, vdiv_mv_trig)`） |

**AUTO 逐通道**

- `App_AutoPickVdiv(mv)`：只用一帧的 min/max，从最灵敏档起找第一个
  `Vpp ≤ 6 格 且 |峰值|×2 ≤ 9 格` 的档，找不到用最粗档。
- `App_AutoVdiv(mv1, mv2)`：按显示模式决定哪些通道参与（CH1 单显跳过 CH2，
  CH2 单显跳过 CH1，FFT 模式只算当前通道），有通道改档才 `App_ApplyRanges() + App_ClampLevel()`。

**界面**

- 状态栏 `StatusBar_Info`：删掉 `ac_dc/range/vdiv_mv`，改为
  `vdiv_mv1/vdiv_mv2/range1/range2`；V/div 区（x=28…84）用通道色并列画两个紧凑值
  （`500m` / `1.0V` / `10V`），量程字母区（x=210、222）用通道色各画一个 `S`/`W`。
  耦合方式仍只能靠硬件跳线判断、软件读不到，所以去掉了写死的 `DC`，状态栏不显示耦合。
- 菜单 `ScopeMenu_Info`：删掉单值 `vdiv_mv`，改为 `vdiv_mv1/vdiv_mv2`；
  V/div 行标签 `V/div C%u`（当前通道），数值并列显示两路（`500m/2.0V`）。
- **KEY1 在菜单里任何一行都有效**（原先只在 `Cal` 行），切通道后自动 `App_ClampLevel()`。

**代价与边界**

- FLASH 69 660 → **70 088 B / 6.68 %**（+428 B），RAM 59 680 → **59 688 B / 45.54 %**（+8 B），
  CCMRAM 0；构建 `EXIT=0`、`-Wall` 零警告。
- 两路量程独立后，**跳线乙必须分别对应该路的量程字母**；对不上会表现为该路波形突然
  压缩或顶到屏幕外（与旧版单量程时的现象相同，只是现在分开判断）。
- 标定仍按"该通道 + 该通道当前量程"进行，不会因为另一路换档而失效。

**验证状态**：ARM 构建 + 静态审查，**未上板**。上板重点看：两路设不同档位时状态栏两个值/
两个字母是否各自正确；KEY1 切通道后编码器1 是否只改当前那一路；菜单 `V/div C1` 行两路值
是否同时更新；两路跳线分别对应后读数是否正常。

**经验**

- "共用还是独立"不是技术问题而是信息呈现问题：把两个值都显示出来，就不需要靠"强制共用"
  来避免歧义。
- "当前通道"是一个消费者集合：参数栏、触发源、编码器1、菜单 V/div、标定目标、AUTO……
  改语义时列一张消费者表逐个对齐（与 §15.8 是同一类问题），比按现象逐个修更稳。
