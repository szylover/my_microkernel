#pragma once

#include <stdint.h>

/*
 * irq.h — 外设 IRQ（来自 PIC）统一入口与分发
 *
 * 我们把 IRQ0..IRQ15 remap 到 IDT 向量 0x20..0x2F。
 * 汇编 stub 会把向量号作为 int_no 传给 C（与异常 regs 布局保持一致）。
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct irq_regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} irq_regs_t;

typedef void (*irq_handler_t)(const irq_regs_t* r);

/* 安装一个 IRQ handler（irq: 0..15）。 */
void irq_install_handler(uint8_t irq, irq_handler_t handler);

/* 由汇编 stubs 调用的统一 C 入口。 */
void irq_handler_c(const irq_regs_t* r);

#ifdef __cplusplus
}
#endif
