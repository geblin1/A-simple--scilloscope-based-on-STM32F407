set(CMAKE_SYSTEM_NAME               Generic)
set(CMAKE_SYSTEM_PROCESSOR          arm)

set(CMAKE_C_COMPILER_ID GNU)
set(CMAKE_CXX_COMPILER_ID GNU)

# Some default GCC settings
# arm-none-eabi- must be part of path environment
set(TOOLCHAIN_PREFIX                arm-none-eabi-)

# Fallback: the STM32Cube (STM32CubeMX) extension installs the GNU Arm toolchain under
# %LOCALAPPDATA%/stm32cube/bundles/gnu-tools-for-stm32/<version>/bin but does not add it
# to PATH, which makes "arm-none-eabi-gcc not found" fail the configure step. If the
# toolchain is not reachable, prepend the newest bundle found there to PATH for this
# configure run. No effect when the toolchain is already available.
find_program(_STM32_ARM_GCC arm-none-eabi-gcc)
if(WIN32 AND NOT _STM32_ARM_GCC AND NOT "$ENV{PATH}" MATCHES "gnu-tools-for-stm32")
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}" _STM32_LOCALAPPDATA)
    file(GLOB _STM32_ARM_GCC_BUNDLES "${_STM32_LOCALAPPDATA}/stm32cube/bundles/gnu-tools-for-stm32/*")
    if(_STM32_ARM_GCC_BUNDLES)
        list(SORT _STM32_ARM_GCC_BUNDLES COMPARE NATURAL ORDER DESCENDING)
        foreach(_bundle IN LISTS _STM32_ARM_GCC_BUNDLES)
            if(EXISTS "${_bundle}/bin/arm-none-eabi-gcc.exe")
                set(ENV{PATH} "${_bundle}/bin;$ENV{PATH}")
                message(STATUS "arm-none-eabi-gcc not found on PATH - added STM32Cube bundle to PATH: ${_bundle}/bin")
                break()
            endif()
        endforeach()
    else()
        message(WARNING "arm-none-eabi-gcc not found on PATH and no STM32Cube gnu-tools-for-stm32 bundle was found.")
    endif()
endif()
set(CMAKE_C_COMPILER                ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER              ${CMAKE_C_COMPILER})
set(CMAKE_CXX_COMPILER              ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_LINKER                    ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_OBJCOPY                   ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_SIZE                      ${TOOLCHAIN_PREFIX}size)

set(CMAKE_EXECUTABLE_SUFFIX_ASM     ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C       ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX     ".elf")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# MCU specific flags
set(TARGET_FLAGS "-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard ")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${TARGET_FLAGS}")
set(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS} -x assembler-with-cpp -MMD -MP")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fdata-sections -ffunction-sections -fstack-usage")

# The cyclomatic-complexity parameter must be defined for the Cyclomatic complexity feature in STM32CubeIDE to work.
# However, most GCC toolchains do not support this option, which causes a compilation error; for this reason, the feature is disabled by default.
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fcyclomatic-complexity")

set(CMAKE_C_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_C_FLAGS_RELEASE "-Os -g0")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -g0")

set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fno-rtti -fno-exceptions -fno-threadsafe-statics")

set(CMAKE_EXE_LINKER_FLAGS "${TARGET_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T \"${CMAKE_SOURCE_DIR}/STM32F407xx_FLASH.ld\"")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --specs=nano.specs")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-Map=${CMAKE_PROJECT_NAME}.map -Wl,--gc-sections")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--print-memory-usage")
set(TOOLCHAIN_LINK_LIBRARIES "m")