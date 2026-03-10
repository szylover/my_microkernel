#include "irq.h"

#include "pic.h"
#include "printk.h"

/*
 * IRQ 分发表：
 * - 下标 0..15 对应 IRQ0..IRQ15
 * - handler 只做设备逻辑
 * - EOI 在 irq_handler_c 里统一发送（避免每个驱动重复）
 */

static irq_handler_t g_irq_handlers[16];

void irq_install_handler(uint8_t irq, irq_handler_t handler) {
    if (irq >= 16) {
        return;
    }
    g_irq_handlers[irq] = handler;
}

void irq_handler_c(const irq_regs_t* r) {
    if (!r) {
        return;
    }

    /* PIC remap: vector = 0x20 + irq */
    if (r->int_no < 0x20 || r->int_no > 0x2F) {
        printk("[irq] unexpected vector=%u\n", r->int_no);
        return;
    }

    uint8_t irq = (uint8_t)(r->int_no - 0x20u);

    irq_handler_t handler = (irq < 16) ? g_irq_handlers[irq] : (irq_handler_t)0;
    if (handler) {
        handler(r);
    }

    /* 通知 PIC：本次 IRQ 处理完成。 */
    pic_send_eoi(irq);
}
