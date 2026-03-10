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
