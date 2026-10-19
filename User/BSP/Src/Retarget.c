/*
 * printf 重定向到 USART1（PA9 = TX / PA10 = RX，115200 8N1）
 *
 * printf 最终会调用 newlib 的 _write()。Core/Src/syscalls.c 里的 _write 是
 * 弱符号（__attribute__((weak))），这里重写成强符号把它覆盖掉，输出就走到
 * USART1 了；调用点不用改任何代码，也不需要额外的初始化函数。
 *
 * 验证方法（链接后）：
 *   arm-none-eabi-nm *.elf | grep _write   ->   T _write
 *   T 表示生效的是本文件的强符号，而不是 syscalls.c 里的 W（弱符号）。
 */
#include "main.h"
#include "usart.h"

int _write(int file, char *ptr, int len)
{
    int total = len;

    (void)file;

    /* HAL 的长度参数是 uint16_t，长数据分块发送，避免被截断 */
    while ((ptr != NULL) && (len > 0))
    {
        uint16_t n = (len > 1024) ? (uint16_t)1024 : (uint16_t)len;

        if (HAL_UART_Transmit(&huart1, (uint8_t *)ptr, n, 100U) != HAL_OK)
        {
            return -1;
        }
        ptr += n;
        len -= n;
    }

    return total;
}
