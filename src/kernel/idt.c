#include "idt.h"

#include <stddef.h>

#include "gdt.h"
#include "printk.h"

/*
 * IDT gate descriptor（8 字节）的布局（32-bit 模式）：
 *
 *  bits 0..15   : offset_low   (handler 地址低 16 位)
 *  bits 16..31  : selector     (代码段选择子，通常是内核代码段 0x08)
 *  bits 32..39  : zero         (必须为 0)
 *  bits 40..47  : type_attr    (P/DPL/Type)
 *  bits 48..63  : offset_high  (handler 地址高 16 位)
 *
 * type_attr 常用：
 * - 0x8E = 10001110b
 *   P=1, DPL=0, Type=1110 (32-bit interrupt gate)
 */

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  type_attr;
    uint16_t offset_high;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} idtr_t;

static idt_entry_t idt[256];
static idtr_t idtr;

static inline void lidt(const idtr_t* idtr_ptr) {
    __asm__ volatile("lidt (%0)" : : "r"(idtr_ptr));
}

static void idt_set_gate(uint8_t vector, void (*handler)(void), uint16_t selector, uint8_t type_attr) {
    uint32_t addr = (uint32_t)(uintptr_t)handler;
    idt[vector].offset_low = (uint16_t)(addr & 0xFFFF);
    idt[vector].selector = selector;
    idt[vector].zero = 0;
    idt[vector].type_attr = type_attr;
    idt[vector].offset_high = (uint16_t)((addr >> 16) & 0xFFFF);
}

/*
 * 汇编 ISR stubs（在 isr_stubs.asm 里定义）。
 * 我们会把 0..31 全部装进 IDT。
 */
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

/*
 * 从汇编 stub 传入的寄存器快照。
 *
 * 这个布局需要和 isr_common_stub 里 push 的顺序严格匹配。
 * 我们采用常见的 OSDev 风格：
 *   push gs, fs, es, ds
 *   pushad
 *   push int_no
 *   push err_code
 *   (CPU 自动压入 eip, cs, eflags ...)
 *
 * 注意：pushad 会把“进入 pushad 前的 ESP 值”也压栈，所以这里的 esp 字段
 * 是一个“当时的值快照”，不是当前栈指针。
 */

typedef struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} regs_t;

static const char* exception_name(uint32_t v) {
    switch (v) {
        case 0:  return "#DE Divide Error";
        case 1:  return "#DB Debug";
        case 2:  return "NMI";
        case 3:  return "#BP Breakpoint";
        case 4:  return "#OF Overflow";
        case 5:  return "#BR BOUND Range";
        case 6:  return "#UD Invalid Opcode";
        case 7:  return "#NM Device Not Available";
        case 8:  return "#DF Double Fault";
        case 9:  return "Coprocessor Segment Overrun";
        case 10: return "#TS Invalid TSS";
        case 11: return "#NP Segment Not Present";
        case 12: return "#SS Stack-Segment Fault";
        case 13: return "#GP General Protection";
        case 14: return "#PF Page Fault";
        case 15: return "Reserved";
        case 16: return "#MF x87 FPU Floating-Point";
        case 17: return "#AC Alignment Check";
        case 18: return "#MC Machine Check";
        case 19: return "#XM SIMD Floating-Point";
        default: return "CPU Exception";
    }
}

/*
 * 汇编 stub 会调用这个函数。
 *
 * 为什么要把异常处理放在 C：
 * - 方便打印/解码（异常名、错误码、寄存器）
 * - 后面可以更容易拓展：page fault 解码 CR2、#GP 解码 selector 等
 */
void isr_handler_c(const regs_t* r) {
    printk(
        "\n[isr] %s vector=%u err=0x%08x eip=0x%08x\n",
        exception_name(r->int_no),
        r->int_no,
        r->err_code,
        r->eip
    );

    /*
     * 对 breakpoint（int3）我们允许返回，这样可以用它做“自检”。
     * 其他异常先简单 halt，避免继续运行导致更难排查。
     */
    if (r->int_no == 3) {
        return;
    }

    /* 进入这里说明发生了“不可恢复”的 CPU 异常。 */
    printk("Kernel panic: ");
    printk("%s\n", exception_name(r->int_no));

    for (;;) {
        __asm__ volatile("hlt");
    }
}

void idt_init(void) {
    /* 清空 IDT（未设置的向量默认 0，会触发 #GP，便于发现缺失）。 */
    for (size_t i = 0; i < 256; i++) {
        idt[i] = (idt_entry_t){0};
    }

    /* 装载 CPU 异常 0..31 */
    const uint8_t INT_GATE = 0x8E;
    idt_set_gate(0,  isr0,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(1,  isr1,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(2,  isr2,  GDT_KERNEL_CODE_SEL, INT_GATE);
    /* breakpoint 通常允许 ring3 触发（DPL=3）。现在先保留学习注释，后面做用户态再改。 */
    idt_set_gate(3,  isr3,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(4,  isr4,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(5,  isr5,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(6,  isr6,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(7,  isr7,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(8,  isr8,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(9,  isr9,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(10, isr10, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(11, isr11, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(12, isr12, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(13, isr13, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(14, isr14, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(15, isr15, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(16, isr16, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(17, isr17, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(18, isr18, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(19, isr19, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(20, isr20, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(21, isr21, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(22, isr22, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(23, isr23, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(24, isr24, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(25, isr25, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(26, isr26, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(27, isr27, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(28, isr28, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(29, isr29, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(30, isr30, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(31, isr31, GDT_KERNEL_CODE_SEL, INT_GATE);

    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base = (uint32_t)(uintptr_t)&idt[0];

    /*
     * lidt 生效后，CPU 就会使用我们的 IDT。
     * 注意：这一步不会自动开中断（IF），是否 sti 由你自己决定。
     */
    lidt(&idtr);
}
