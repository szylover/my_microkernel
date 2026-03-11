#include "serial.h"

/*
 * UART 16550 寄存器（以 COM1=0x3F8 为基址）：
 *
 * +0: 数据寄存器 (DATA) / 或 DLAB=1 时为除数低字节 (DLL)
 * +1: 中断使能 (IER) / 或 DLAB=1 时为除数高字节 (DLM)
 * +2: 中断识别 (IIR) / FIFO 控制 (FCR)
 * +3: 线路控制 (LCR) —— 位 7 是 DLAB
 * +4: Modem 控制 (MCR)
 * +5: 线路状态 (LSR) —— 位 5 表示发送保持寄存器空（可以发下一个字节）
 *
 * 我们只需要最小的“轮询发送”能力：等待 LSR bit5，然后写 DATA。
 */

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

enum {
    COM1 = 0x3F8,
};

static int serial_transmit_empty(void) {
    return (inb((uint16_t)(COM1 + 5)) & 0x20) != 0;
}

static int serial_received(void) {
    /* LSR bit0 = Data Ready */
    return (inb((uint16_t)(COM1 + 5)) & 0x01) != 0;
}

void serial_putc(char c) {
    /* Most serial terminals expect CRLF for newlines. If we only send '\n'
     * (LF), the cursor may move down but stay in the same column, which makes
     * ASCII art boxes look misaligned.
     */
    if (c == '\n') {
        while (!serial_transmit_empty()) {
            // busy wait
        }
        outb(COM1, (uint8_t)'\r');
    }
    while (!serial_transmit_empty()) {
        // busy wait
    }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char* s) {
    for (; *s; s++) {
        serial_putc(*s);
    }
}

void serial_write_hex32(uint32_t v) {
    static const char* hex = "0123456789ABCDEF";
    serial_write("0x");
    for (int i = 7; i >= 0; i--) {
        serial_putc(hex[(v >> (i * 4)) & 0xF]);
    }
}

void serial_init(void) {
    /*
     * 初始化串口：
     * - 关闭中断（我们还没做 PIC/IDT IRQ）
     * - 设置波特率（通过除数 latch：115200 / divisor）
     * - 8N1
     * - 开 FIFO
     */
    outb(COM1 + 1, 0x00); // Disable all interrupts
    outb(COM1 + 3, 0x80); // Enable DLAB
    outb(COM1 + 0, 0x03); // Divisor low byte (115200/3 = 38400 baud)
    outb(COM1 + 1, 0x00); // Divisor high byte
    outb(COM1 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7); // Enable FIFO, clear, 14-byte threshold
    outb(COM1 + 4, 0x0B); // IRQs enabled (in UART), RTS/DSR set
}

int serial_try_getc(char* out) {
    if (!out) {
        return 0;
    }

    if (!serial_received()) {
        return 0;
    }

    *out = (char)inb(COM1);
    return 1;
}

char serial_getc(void) {
    char c;
    while (!serial_try_getc(&c)) {
        __asm__ volatile("pause");
    }
    return c;
}
