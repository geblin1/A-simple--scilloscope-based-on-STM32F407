cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
# NOTE: CMake >= 4.0 rejects a non-ASCII executable target name
# ("The target name ... is reserved or not valid for certain CMake features").
# CMAKE_PROJECT_NAME is therefore an ASCII identifier, which is used for the target
# and for the .elf/.map file names; the original Chinese title is kept as a display
# name only.
set(CMAKE_PROJECT_DISPLAY_NAME 基于STM32F407ZGT6的简易示波器)
set(CMAKE_PROJECT_NAME STM32F407ZGT6_Scope_SignalGenerator_Multimeter)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})
message("Project display name: " ${CMAKE_PROJECT_DISPLAY_NAME})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    # （User/ 下的头文件目录由下面的 "User/ 目录自动收集" 自动加入，无需手写）
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# ============================================================================
# User/ 目录自动收集：往 User/ 下新增文件后不需要再改本文件
# ----------------------------------------------------------------------------
# 规则：
#   * User/ 下（含任意层子目录）的 .c/.cpp/.cc/.s/.S  -> 自动加入编译
#   * User/ 下任何存放 .h/.hpp 的目录                  -> 自动成为头文件搜索路径
#     （User/ 自身也在搜索路径里，所以也能写 "BSP/Inc/xxx.h" 这种带路径的包含）
#   * 增删文件后**不需要**手动重新 configure：CONFIGURE_DEPENDS 让 Ninja 在每次
#     构建开始时校验这些目录，内容变了就自动重跑 CMake 再增量编译。
#
# 只影响 User/，不会碰 Core/、Drivers/、ThirdParty/。本文件是 CubeMX 的
# "generated only once"，重新生成代码不会覆盖这段逻辑。
# ============================================================================
set(USER_DIR "${CMAKE_SOURCE_DIR}/User")

# 递归收集源文件：新增 .c 文件后，下一次构建自动参与编译
file(GLOB_RECURSE USER_SOURCES CONFIGURE_DEPENDS
    "${USER_DIR}/*.c"
    "${USER_DIR}/*.cpp"
    "${USER_DIR}/*.cc"
    "${USER_DIR}/*.s"
    "${USER_DIR}/*.S"
)
# Windows 文件系统不区分大小写，*.s 与 *.S 会命中同一个文件，这里去重
list(REMOVE_DUPLICATES USER_SOURCES)

# 收集头文件所在目录，全部加入 include 路径（不必再手写 include_directories）
file(GLOB_RECURSE USER_HEADERS CONFIGURE_DEPENDS
    "${USER_DIR}/*.h"
    "${USER_DIR}/*.hpp"
)

set(USER_INCLUDE_DIRS "${USER_DIR}")
foreach(_user_hdr IN LISTS USER_HEADERS)
    get_filename_component(_user_hdr_dir "${_user_hdr}" DIRECTORY)
    list(APPEND USER_INCLUDE_DIRS "${_user_hdr_dir}")
endforeach()
list(REMOVE_DUPLICATES USER_INCLUDE_DIRS)
unset(_user_hdr)
unset(_user_hdr_dir)

if(USER_SOURCES)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${USER_SOURCES})
    list(LENGTH USER_SOURCES _user_src_count)
    message(STATUS "User/ 自动收集: ${_user_src_count} 个源文件")
    foreach(_user_src IN LISTS USER_SOURCES)
        message(STATUS "    + ${_user_src}")
    endforeach()
    unset(_user_src)
    unset(_user_src_count)
else()
    message(STATUS "User/ 自动收集: ${USER_DIR} 下暂无源文件")
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${USER_INCLUDE_DIRS})
message(STATUS "User/ 头文件搜索路径: ${USER_INCLUDE_DIRS}")

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# ============================================================================
# FreeRTOS Kernel V11.1.0（手工添加；本文件是 "generated only once"，
# CubeMX 不会重写，所以下面的集成长久有效）
# ----------------------------------------------------------------------------
# * 内核源码在 ThirdParty/FreeRTOS/Source，而不是 Middlewares/：
#   CubeMX 每次重新生成都会清空 Middlewares/，放那里的内核会丢。
# * 移植层用 portable/GCC/ARM_CM4F：本工程编译参数是
#   -mfpu=fpv4-sp-d16 -mfloat-abi=hard（见 cmake/gcc-arm-none-eabi.cmake），
#   必须用带 FPU 上下文的 Cortex-M4F 移植层（参考工程用的 ARM_CM3 不适用）。
# * 配置文件在 Core/Inc/FreeRTOSConfig.h。
# * 详细说明见 ThirdParty/FreeRTOS/README.md。
# ============================================================================
set(FREERTOS_DIR      "${CMAKE_SOURCE_DIR}/ThirdParty/FreeRTOS/Source")
set(FREERTOS_PORT_DIR "${FREERTOS_DIR}/portable/GCC/ARM_CM4F")

set(FREERTOS_SOURCES
    "${FREERTOS_DIR}/croutine.c"
    "${FREERTOS_DIR}/event_groups.c"
    "${FREERTOS_DIR}/list.c"
    "${FREERTOS_DIR}/queue.c"
    "${FREERTOS_DIR}/stream_buffer.c"
    "${FREERTOS_DIR}/tasks.c"
    "${FREERTOS_DIR}/timers.c"
    "${FREERTOS_PORT_DIR}/port.c"
    "${FREERTOS_DIR}/portable/MemMang/heap_4.c"
)

# 内核文件缺失时立刻给出可读的错误，而不是让链接器报一堆 undefined reference
foreach(_frtos_src IN LISTS FREERTOS_SOURCES)
    if(NOT EXISTS "${_frtos_src}")
        message(FATAL_ERROR
            "FreeRTOS 内核文件缺失: ${_frtos_src}\n"
            "请确认 ThirdParty/FreeRTOS/Source 完整（见 ThirdParty/FreeRTOS/README.md）。")
    endif()
endforeach()
unset(_frtos_src)

add_library(FreeRTOS OBJECT)
target_sources(FreeRTOS PRIVATE ${FREERTOS_SOURCES})
target_include_directories(FreeRTOS PUBLIC
    "${FREERTOS_DIR}/include"
    "${FREERTOS_PORT_DIR}"
)
# stm32cubemx 是 INTERFACE 库，提供 Core/Inc（FreeRTOSConfig.h 就在那里）
target_link_libraries(FreeRTOS PUBLIC stm32cubemx)

# 应用层：RTOS 入口与钩子函数（手工维护，CubeMX 不生成）
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Src/freertos_app.c"
    "${CMAKE_SOURCE_DIR}/Core/Src/freertos_hooks.c"
)

# 让应用代码能找到 FreeRTOS.h / task.h / queue.h / timers.h
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${FREERTOS_DIR}/include"
    "${FREERTOS_PORT_DIR}"
)

target_link_libraries(${CMAKE_PROJECT_NAME}
    FreeRTOS
)

# ----------------------------------------------------------------------------
# 让 RTOS 接管 SVC / PendSV / SysTick 三个异常
# ----------------------------------------------------------------------------
# CubeMX 每次重新生成 Core/Src/stm32f4xx_it.c 都会再次定义这三个中断服务函数
# （空函数体），与移植层 port.c 里的实现重复，会导致重复符号 / 第一次上下文切换
# 就死机。这里**不修改** stm32f4xx_it.c（改了也会被 CubeMX 覆盖），而是只对该
# 源文件加编译宏、把这三个名字在本编译单元内改掉：
#   SVC_Handler / PendSV_Handler / SysTick_Handler -> CubeMX_Unused_*
# 于是 FreeRTOSConfig.h 里改名过的 vPortSVCHandler / xPortPendSVHandler /
# xPortSysTickHandler 成为唯一实现，正好对上启动文件 startup_stm32f407xx.s
# 向量表中的 .word SVC_Handler / .word PendSV_Handler / .word SysTick_Handler。
# 每次 CubeMX 重新生成代码之后都自动成立，无需人工修补。
#
# 前提：CubeMX 里 HAL 时基必须保持 TIM7（Core/Src/stm32f4xx_hal_timebase_tim.c），
#       SysTick 才能完整让给 RTOS。
# ----------------------------------------------------------------------------
set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/Core/Src/stm32f4xx_it.c"
    TARGET_DIRECTORY ${CMAKE_PROJECT_NAME}
    PROPERTIES COMPILE_DEFINITIONS
        "SVC_Handler=CubeMX_Unused_SVC_Handler;PendSV_Handler=CubeMX_Unused_PendSV_Handler;SysTick_Handler=CubeMX_Unused_SysTick_Handler"
)
