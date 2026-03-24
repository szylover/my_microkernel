#pragma once

#include <stdint.h>

/*
 * IDT (Interrupt Descriptor Table) — x86 中断/异常的“入口表”。
 *
 * 学习要点：
 * - CPU 在发生异常（#UD/#GP/#PF...）或执行 int 指令时，会用向量号 N（0..255）
 *   在 IDT 查找 gate descriptor，并跳转到对应 handler。
 * - 对我们来说，IDT 的价值是“可观测性”：当你写错了某个寄存器/页表/指令，
 *   不再是黑屏/重启，而是串口能打印出是哪种异常、错误码是什么。
 *
 * 这里我们只做最小版本：
 * - 安装 0..31 号向量（CPU 异常）
 * - 使用 32-bit interrupt gate（会自动清 IF，避免中断嵌套把早期栈打爆）
 */

void idt_init(void);

/*
 * idt_install_gate — 安装任意 DPL 的 IDT 门描述符
 *
 * 供内核其他子系统（如系统调用后端）在 idt_init() 之后动态安装 IDT gate。
 *
 * @param vector    中断/异常向量号 (0..255)
 * @param handler   汇编处理入口地址
 * @param selector  代码段选择子（通常 GDT_KERNEL_CODE_SEL = 0x08）
 * @param type_attr 类型属性字节
 *                  - 0x8E = P=1, DPL=0, 32-bit interrupt gate（内核异常/IRQ）
 *                  - 0xEE = P=1, DPL=3, 32-bit interrupt gate（用户态可触发，如 int 0x80）
 *
 * [WHY] DPL=3 允许 Ring 3 代码用软件 int 指令触发该向量。
 *   DPL=0 的向量只能由内核（CPL=0）或硬件异常触发；用户态软件触发会引发 #GP。
 *
 * [BITFIELDS] type_attr = 0xEE:
 *   bit 7   (P)    = 1   → present（此描述符有效）
 *   bits 6-5 (DPL) = 11  → privilege level 3（Ring 3 可触发）
 *   bit 4   (S)    = 0   → 系统描述符（非代码/数据段）
 *   bits 3-0 (Type)= 1110 → 32-bit interrupt gate（进入时 CPU 清 IF）
 */
void idt_install_gate(uint8_t vector, void (*handler)(void),
                      uint16_t selector, uint8_t type_attr);
