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

/*
 * 串口输入（轮询）。
 *
 * 说明：当 QEMU 使用 `-serial stdio` 时，宿主终端输入会进入来宾 COM1。
 * 早期我们不做串口 IRQ（更复杂），先用轮询即可让 shell 在“串口窗口”里交互。
 */

/*
 * serial_try_getc:
 * - 若当前没有字符可读，返回 0
 * - 若读到一个字符，写入 *out 并返回 1
 */
int serial_try_getc(char* out);

/* serial_getc: 阻塞直到读到一个字符并返回。 */
char serial_getc(void);
