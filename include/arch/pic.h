#pragma once

#include <stdint.h>

/*
 * pic.h — 8259A Programmable Interrupt Controller (PIC)
 *
 * 为什么需要 PIC：
 * - CPU 的外部硬件中断（IRQ0..IRQ15）由 PIC 汇聚后送到 CPU 的 INTR 引脚。
 * - 在我们没有 APIC 的阶段，必须配置 PIC 才能收到键盘/时钟等硬中断。
 *
 * 关键步骤：
 * 1) remap：把 IRQ 映射到 IDT 0x20..0x2F，避免与 CPU 异常 0..31 冲突。
 * 2) mask：按需打开某些 IRQ（例如键盘 IRQ1）。
 * 3) EOI：中断处理完成后发送 End Of Interrupt，否则 PIC 不会继续发后续中断。
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 将 master/slave PIC remap 到 0x20/0x28，并初始化 mask（默认全关）。 */
void pic_init(void);

/* 发送 EOI（irq: 0..15）。 */
void pic_send_eoi(uint8_t irq);

/* 设置/清除单个 IRQ mask（irq: 0..15）。 */
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);

#ifdef __cplusplus
}
#endif
