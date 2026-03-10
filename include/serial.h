#pragma once

#include <stdint.h>

/*
 * 串口（UART 16550）最小驱动：用于在裸机环境下输出调试信息。
 *
 * 为什么要串口：
 * - VGA 打印只能看到少量字符，且后面进入图形/分页后不一定可用。
 * - QEMU 支持把 COM1 重定向到宿主终端（`-serial stdio`），非常适合早期调试。
 *
 * 我们使用 COM1 基址 0x3F8。
 */

void serial_init(void);
void serial_putc(char c);
void serial_write(const char* s);
void serial_write_hex32(uint32_t v);
