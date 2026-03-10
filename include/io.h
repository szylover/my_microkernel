#pragma once

#include <stdint.h>

/*
 * io.h — x86 I/O port 访问
 *
 * 学习级要点：
 * - `inb/outb` 是访问 I/O space（端口空间）的指令，不是内存读写。
 * - 典型外设：8259 PIC、8042/PS2 控制器、UART 16550 等都通过端口访问。
 * - 这些函数必须是 `static inline`，避免额外调用开销，并保证 freestanding 环境可用。
 */

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void) {
    /*
     * 传统做法：向 0x80 输出任意值，产生一个很短的延时。
     * 在现代硬件/虚拟机里这更多是“语义上的等待”，对 PIC 初始化序列很常用。
     */
    outb(0x80, 0);
}
