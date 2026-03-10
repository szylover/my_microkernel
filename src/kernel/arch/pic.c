#include "pic.h"

#include "io.h"

/*
 * 8259 PIC 端口定义
 * - master: 0x20/0x21
 * - slave : 0xA0/0xA1
 */

enum {
    PIC1_CMD  = 0x20,
    PIC1_DATA = 0x21,
    PIC2_CMD  = 0xA0,
    PIC2_DATA = 0xA1,

    PIC_EOI = 0x20,

    ICW1_INIT = 0x10,
    ICW1_ICW4 = 0x01,
    ICW4_8086 = 0x01,
};

static uint8_t pic_read_mask(uint16_t data_port) {
    return inb(data_port);
}

static void pic_write_mask(uint16_t data_port, uint8_t mask) {
    outb(data_port, mask);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  line = (uint8_t)(irq & 7u);

    uint8_t mask = pic_read_mask(port);
    mask |= (uint8_t)(1u << line);
    pic_write_mask(port, mask);
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  line = (uint8_t)(irq & 7u);

    uint8_t mask = pic_read_mask(port);
    mask &= (uint8_t)~(1u << line);
    pic_write_mask(port, mask);
}

void pic_send_eoi(uint8_t irq) {
    /*
     * 如果中断来自 slave（IRQ8..15），必须先通知 slave 再通知 master。
     * 原因：slave 是挂在 master 的 IRQ2 上的级联。
     */
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

void pic_init(void) {
    /*
     * PIC remap（经典序列）：
     * - 保存当前 mask（可选）
     * - 发送 ICW1: INIT + ICW4
     * - 发送 ICW2: vector offset（master=0x20, slave=0x28）
     * - 发送 ICW3: 级联关系（master: slave 在 IRQ2；slave: cascade identity=2）
     * - 发送 ICW4: 8086/88 模式
     *
     * 我们这里选择：初始化后先把所有 IRQ mask 掉（全 1），再由驱动按需打开。
     */
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_CMD, (uint8_t)(ICW1_INIT | ICW1_ICW4));
    io_wait();
    outb(PIC2_CMD, (uint8_t)(ICW1_INIT | ICW1_ICW4));
    io_wait();

    outb(PIC1_DATA, 0x20); /* ICW2: master offset */
    io_wait();
    outb(PIC2_DATA, 0x28); /* ICW2: slave offset */
    io_wait();

    outb(PIC1_DATA, 0x04); /* ICW3: master has slave on IRQ2 (0000 0100) */
    io_wait();
    outb(PIC2_DATA, 0x02); /* ICW3: slave identity = 2 */
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* 默认：先关掉所有 IRQ，避免未安装 handler 的中断进来导致 #GP。 */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    /* 若你想保留引导器 mask，可恢复 a1/a2；现在以“可控”为优先。 */
    (void)a1;
    (void)a2;
}
