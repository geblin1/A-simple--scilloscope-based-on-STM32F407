# FreeRTOS 内核（ThirdParty/FreeRTOS）

本目录是移植进本工程的 **FreeRTOS Kernel V11.1.0**（LTS 版本），内核源码取自
`D:\FreeRTOSv202406.05-LTS\FreeRTOS-LTS\FreeRTOS\FreeRTOS-Kernel`。

目标芯片：**STM32F407ZGT6**（Cortex-M4F，168 MHz 上限，本工程当前跑 HSI 16 MHz）
工具链：GNU Arm Embedded GCC（STM32Cube 扩展的 gnu-tools-for-stm32 bundle）
构建：STM32CubeMX 生成的 CMake 工程（Ninja）

---

## 1. 目录结构

```
ThirdParty/FreeRTOS/
├── README.md                     ← 本文件
└── Source/
    ├── croutine.c                协程（本工程 configUSE_CO_ROUTINES=0，不参与运行）
    ├── event_groups.c            事件组
    ├── list.c                    内核链表
    ├── queue.c                   队列 / 信号量 / 互斥量
    ├── stream_buffer.c           流缓冲 / 消息缓冲
    ├── tasks.c                   调度器与任务管理
    ├── timers.c                  软件定时器
    ├── LICENSE.md
    ├── include/                  内核公开头文件（FreeRTOS.h、task.h、queue.h …）
    └── portable/
        ├── GCC/ARM_CM4F/         ← Cortex-M4F 移植层（port.c / portmacro.h）
        └── MemMang/heap_4.c      ← 内存管理方案 4（带碎片合并）
```

配置文件 **不在** 这里，而在 `Core/Inc/FreeRTOSConfig.h`（与参考工程一致）。

---

## 2. 为什么放在 ThirdParty/ 而不是 Middlewares/

**STM32CubeMX 每次重新生成代码都会清空并重建 `Middlewares/` 目录**，放在那里的
内核会被直接删掉，而 `ThirdParty/` 不在 CubeMX 的管理范围内，可以长期保留。
（参考工程 `D:\平衡车\平衡车程序` 就是因此把内核放在 `ThirdParty/FreeRTOS`，
它的 README 里写着"本项目已因此丢失过一次内核"。）

同理，下面这些文件都是**手工维护、CubeMX 不会生成也不会删除**的：

| 文件 | 作用 |
| --- | --- |
| `Core/Inc/FreeRTOSConfig.h` | 内核裁剪与配置 |
| `Core/Src/freertos_app.c` | RTOS 应用入口与示例任务（对应参考工程的 `freertos_demo.c`） |
| `Core/Src/freertos_hooks.c` | 内核要求的钩子（栈溢出 / 堆失败 / 断言） |
| 顶层 `CMakeLists.txt` 末段 | FreeRTOS 的编译集成与中断接管 |

---

## 3. 与参考工程（平衡车，STM32F103C8T6）的差异

| 项目 | 参考工程（F103C8T6） | 本工程（F407ZGT6） | 原因 |
| --- | --- | --- | --- |
| 移植层 | `portable/GCC/ARM_CM3` | **`portable/GCC/ARM_CM4F`** | 本工程编译参数是 `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`（见 `cmake/gcc-arm-none-eabi.cmake`）。CM3 移植层不带 FPU 上下文保存，硬浮点下任务切换会丢浮点寄存器 |
| `configCPU_CLOCK_HZ` | 常量 `72000000` | **变量 `SystemCoreClock`** | 本工程当前是 HSI 16 MHz 且 PLL 关闭；写死常量会与实际差 10.5 倍。`SystemCoreClock` 由 `HAL_RCC_ClockConfig()` 自动刷新，以后在 CubeMX 里改成 HSE+PLL(168 MHz) 也不用改配置 |
| HAL 时基 | TIM1（`stm32f1xx_hal_timebase_tim.c`） | **TIM7**（`stm32f4xx_hal_timebase_tim.c`） | 两者都必须是"非 SysTick"，SysTick 才能完整让给 RTOS。本工程 CubeMX 已默认用 TIM7 (`TICK_INT_PRIORITY = 15`) |
| SVC/PendSV/SysTick 接管 | 手工注释掉 `stm32f1xx_it.c` 里的中断函数 | **用编译宏改名**（见第 5 节） | 参考工程的做法在 CubeMX 重新生成代码后会被覆盖；本工程的做法每次自动生效 |
| 应用入口 | `Core/Src/freertos_demo.c` → `freertos_start()` | `Core/Src/freertos_app.c` → `freertos_app_start()` | 命名不同，形式相同（见第 4 节） |
| 内核位置 | `ThirdParty/FreeRTOS/Source` | 同左（保持一致） | — |

---

## 4. 集成点（改代码时看这里）

1. **顶层 `CMakeLists.txt` 末段**（CubeMX 不会重写该文件）：
   - `FREERTOS_SOURCES` 列表 + `add_library(FreeRTOS OBJECT)`，移植层指向 `portable/GCC/ARM_CM4F`；
   - 文件缺失时 `message(FATAL_ERROR ...)`，避免链接期一堆 undefined reference；
   - 把 `Core/Src/freertos_app.c`、`Core/Src/freertos_hooks.c` 加入 `${CMAKE_PROJECT_NAME}`；
   - 给 `Core/Src/stm32f4xx_it.c` 单独加三个改名宏（第 5 节）。
2. **`Core/Inc/FreeRTOSConfig.h` 末尾三行**把移植层的
   `vPortSVCHandler / xPortPendSVHandler / xPortSysTickHandler`
   改名成 CMSIS 标准名 `SVC_Handler / PendSV_Handler / SysTick_Handler`。
3. **`Core/Src/main.c`**：`#include "freertos_app.h"`（USER CODE BEGIN Includes），
   并在 `MX_GPIO_Init()` 之后的 **USER CODE BEGIN 2** 里调用 `freertos_app_start()`。
4. **`Core/Src/freertos_app.c`**：示例任务（生产者 / 消费者 / 秒任务 + 一个软件定时器）。
   本工程当前在 CubeMX 里没有配置 GPIO 引脚，所以示例用**全局计数器**代替 LED，
   调试时把 `g_ulRtos*` 变量加进 Watch 窗口即可判断调度是否在跑。

---

## 5. 中断接管是怎么做到"CubeMX 重生成也不坏"的

冲突点：启动文件 `startup_stm32f407xx.s` 的向量表引用 `SVC_Handler` /
`PendSV_Handler` / `SysTick_Handler`，而 **CubeMX 每次重新生成都会在
`Core/Src/stm32f4xx_it.c` 里再次定义这三个空函数**，与移植层 `port.c` 里的
实现重复 → 重复符号，或第一次任务切换就死机。

本工程**不修改** `stm32f4xx_it.c`（改了也会被覆盖），而是在顶层 `CMakeLists.txt`
里只对该源文件设置编译宏：

```cmake
set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/Core/Src/stm32f4xx_it.c"
    TARGET_DIRECTORY ${CMAKE_PROJECT_NAME}
    PROPERTIES COMPILE_DEFINITIONS
        "SVC_Handler=CubeMX_Unused_SVC_Handler;PendSV_Handler=CubeMX_Unused_PendSV_Handler;SysTick_Handler=CubeMX_Unused_SysTick_Handler"
)
```

这三个空函数在本编译单元里被改名（随后被 `--gc-sections` 回收），
于是 `port.c` 的实现成为唯一强符号，正好对上向量表。
**每次 CubeMX 重新生成代码后都自动成立，不需要任何人工修补。**

---

## 6. 在 CubeMX 里改配置时必须遵守的三条

1. **HAL 时基必须保持 TIM7**（System Core → Timebase Source = TIM7）。
   若改回 SysTick，HAL 与 RTOS 会同时抢 SysTick，`HAL_Delay()` 会永久卡死。
2. **不要在 .ioc 里启用 FreeRTOS 中间件**（Middleware → FREERTOS 保持 Disabled）。
   否则 CubeMX 会再带进一套内核（V10.3.1 + CMSIS-RTOS2）与 `Middlewares/` 目录，
   与 `ThirdParty/FreeRTOS` 冲突，重生成时又会被清空。
3. **任何调用 `...FromISR()` API 的中断，抢占优先级必须 ≥ 5**
   （`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`，对应寄存器值 `0x50`）。
   数值越大优先级越低；优先级 0~4 的中断里调用 RTOS API 会破坏内核临界区。
   本工程 TIM7 的优先级是 15，安全。

另外：不要创建与内核头文件仅大小写不同的文件（例如 `Core/Inc/freertos.h`），
Windows 文件系统下它会抢占 `FreeRTOS.h`。

---

## 7. 已验证的结果

用与 VS Code 相同的 cube-cmake 流程（configure + build）实测通过：

- `cube-cmake --preset Debug` → `Configuring done`，EXIT=0
- `cube-cmake --build build/Debug` → 20/20 目标编译链接成功，EXIT=0
- 内存占用：**RAM 36832 B / 128 KB (28.10%)**（其中 32 KB 是 `configTOTAL_HEAP_SIZE`）、
  CCMRAM 0 B、**FLASH 19400 B / 1 MB (1.85%)**
- `arm-none-eabi-nm` 符号检查：`SVC_Handler=0x080039c0`、`PendSV_Handler=0x08003c40`、
  `SysTick_Handler=0x08003ca8` 均由 `port.c` 提供，`CubeMX_Unused_*` 已被回收
- 向量表检查（`objdump -s -j .isr_vector`）：
  `0x2C=0x080039C1`(SVC)、`0x38=0x08003C41`(PendSV)、`0x3C=0x08003CA9`(SysTick)，
  Thumb 位正确，全部指向 RTOS 实现
- `HAL_SYSTICK_Config` / `HAL_SuspendTick` / `HAL_ResumeTick` 均未参与链接
  → HAL 完全不碰 SysTick，`HAL_InitTick` 的强符号来自 TIM7 时基文件

上电后的自检（不需要示波器）：把下面这些变量加进调试器 Watch 窗口，
`g_ulRtosSecondsCount` 每秒 +1、`g_ulRtosProducedCount`/`g_ulRtosConsumedCount`
每 500 ms 各 +1、`g_ulRtosTimerCount` 每秒 +1，就说明调度器、队列、
软件定时器都在正常工作。`g_ulRtosHeapFreeBytes` 用于判断堆是否够用。

---

## 8. 常见故障排查

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 链接报 `SVC_Handler / PendSV_Handler / SysTick_Handler 重复定义` | 顶层 CMakeLists 的改名宏没生效（例如被删掉，或 `stm32f4xx_it.c` 路径变了） | 检查第 5 节那段 `set_source_files_properties` 是否还在、路径是否存在 |
| 第一个任务切换就进 HardFault，或 `vTaskStartScheduler()` 后不运行 | 向量表未指向 RTOS 实现；或 `configASSERT` 失败 | 用 `nm` 看三个 handler 的地址是否落在 `port.c`；看 `g_pcRtosAssertFile` / `g_ulRtosAssertLine` |
| `HAL_Delay()` 卡死 | HAL 时基被改成了 SysTick | CubeMX 里改回 TIM7 |
| 进 `vApplicationMallocFailedHook()` | `configTOTAL_HEAP_SIZE` 不够（当前 32 KB） | 调大 `FreeRTOSConfig.h` 的 `configTOTAL_HEAP_SIZE`，或减小任务栈 |
| 进 `vApplicationStackOverflowHook()` | 某任务栈太小 | 看 `g_pcStackOverflowTask`，把该任务的栈（`configMINIMAL_STACK_SIZE * n`）调大 |
| 在中断里调 RTOS API 后随机死机 | 中断抢占优先级高于 5 | 把该中断优先级改成 ≥ 5 |

---

## 9. 后续扩展建议

- **分层**：参考工程用 `User/{BSP,App,Algorithm}/{Inc,Src}`。本工程目前只有
  `Core/Src`，需要时可以直接建这三个目录，并在顶层 `CMakeLists.txt` 里加：
  ```cmake
  file(GLOB_RECURSE USER_SOURCES "${CMAKE_SOURCE_DIR}/User/*.c")
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${USER_SOURCES})
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
      "${CMAKE_SOURCE_DIR}/User/BSP/Inc"
      "${CMAKE_SOURCE_DIR}/User/App/Inc"
      "${CMAKE_SOURCE_DIR}/User/Algorithm/Inc")
  ```
- **加任务**：在 `freertos_app.c` 的 `freertos_app_start()` 里 `xTaskCreate()`，
  或新建 `Core/Src/freertos_xxx.c` 并在顶层 CMakeLists 里加到 `${CMAKE_PROJECT_NAME}`。
  三合一系统的自然分工：示波器采样任务（高优先级 + ADC/DMA 中断）、
  信号发生器 DDS 任务（定时器节拍）、万用表测量任务、界面/串口任务（低优先级）。
- **中断与 RTOS 交互**：ADC/DMA 完成中断里用 `xQueueSendFromISR()` /
  `vTaskNotifyGiveFromISR()` 通知采样任务，注意第 6 节第 3 条优先级要求。
