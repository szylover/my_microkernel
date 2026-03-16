#include "idt.h"

#include <stddef.h>

#include "gdt.h"
#include "vma.h"
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
 * 汇编 IRQ stubs（在 irq_stubs.asm 里定义）。
 * 这些向量对应 PIC remap 后的 0x20..0x2F。
 */
extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

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
 * 读取 CR2 寄存器 — Page Fault 时 CPU 自动写入触发故障的虚拟地址。
 *
 * [WHY]
 *   当发生 #PF (vector 14) 时，CR2 保存了试图访问但翻译失败的虚拟地址。
 *   这是调试分页问题的关键信息：它告诉你 **是谁** 引起了 page fault。
 */
static inline uint32_t read_cr2(void) {
    uint32_t val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

/*
 * 解码 Page Fault 错误码 (error code)
 *
 * [BITFIELDS] #PF 错误码格式（由 CPU 自动压栈）：
 *
 *   bit 0 (P)    : 0 = 页面不存在 (not-present)
 *                  1 = 页面存在但权限违规 (protection violation)
 *   bit 1 (W/R)  : 0 = 读操作引起
 *                  1 = 写操作引起
 *   bit 2 (U/S)  : 0 = 在内核态 (CPL=0) 发生
 *                  1 = 在用户态 (CPL=3) 发生
 *   bit 3 (RSVD) : 1 = 页表条目中的保留位被置位
 *   bit 4 (I/D)  : 1 = 取指令引起（instruction fetch）
 *
 * 例子：err=0x00 → 内核态读一个不存在的页
 *       err=0x02 → 内核态写一个不存在的页
 *       err=0x05 → 用户态读一个存在但权限不足的页
 */
static void page_fault_handler(const regs_t* r) {
    uint32_t fault_addr = read_cr2();
    uint32_t err = r->err_code;

    printk("\n======== PAGE FAULT ========\n");
    printk("Faulting address (CR2): 0x%08x\n", fault_addr);
    printk("Error code: 0x%08x\n", err);

    /* 逐位解码 */
    printk("  %s\n", (err & 0x01u) ? "Protection violation (page present)"
                                    : "Page not present");
    printk("  Caused by: %s\n", (err & 0x02u) ? "write" : "read");
    printk("  CPU mode:  %s\n", (err & 0x04u) ? "user (Ring 3)" : "kernel (Ring 0)");
    if (err & 0x08u) {
        printk("  Reserved bit set in page table entry!\n");
    }
    if (err & 0x10u) {
        printk("  Caused by instruction fetch\n");
    }

    printk("EIP: 0x%08x\n", r->eip);

    /*
     * Stage 9 (VMA): 用 VMA 查找故障地址所属的虚拟内存区域
     *
     * [WHY] 通过 VMA 可以判断故障是否"合法"：
     *   - 无 VMA 覆盖 → 访问了未分配的地址空间，一定是 bug
     *   - 有 VMA 但权限不符 → 如写入只读区域，权限违规
     *   - 有 VMA 且权限匹配但页不存在 → 将来可做按需分页 (demand paging)
     *
     * 当前阶段：只增强诊断信息，所有 #PF 仍然 panic。
     * 将来 Stage 11/13 会在此处实现按需分页（检测到合法 VMA 后分配+映射物理页）。
     */
    if (vma_is_ready()) {
        const vm_area_t* vma = vma_find(fault_addr);
        if (vma) {
            printk("VMA: [0x%08x, 0x%08x) '%s' flags=%c%c%c\n",
                   vma->start, vma->end,
                   vma->name ? vma->name : "?",
                   (vma->flags & VMA_READ)  ? 'r' : '-',
                   (vma->flags & VMA_WRITE) ? 'w' : '-',
                   (vma->flags & VMA_EXEC)  ? 'x' : '-');

            /* 检查权限匹配 */
            if ((err & 0x02u) && !(vma->flags & VMA_WRITE)) {
                printk("  -> WRITE to read-only VMA!\n");
            }
            if ((err & 0x10u) && !(vma->flags & VMA_EXEC)) {
                printk("  -> EXEC in non-executable VMA!\n");
            }
            if (!(err & 0x01u)) {
                printk("  -> Page not present in valid VMA (demand paging candidate)\n");
            }
        } else {
            printk("VMA: no VMA covers 0x%08x (invalid access)\n", fault_addr);
        }
    }

    printk("============================\n");
}

/*
 * 汇编 stub 会调用这个函数。
 *
 * 为什么要把异常处理放在 C：
 * - 方便打印/解码（异常名、错误码、寄存器）
 * - #PF 特殊处理：读 CR2、解码错误码
 */
void isr_handler_c(const regs_t* r) {
    /*
     * #PF (Page Fault, vector 14) 需要特殊处理：读 CR2、解码错误码。
     *
     * [WHY] 分离出来方便将来做按需分页（demand paging）时
     *       在此处分配物理页并恢复执行，而非直接 panic。
     */
    if (r->int_no == 14) {
        page_fault_handler(r);
        /* 当前阶段：page fault 是不可恢复的，halt */
        printk("Kernel panic: #PF Page Fault\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    printk(
        "\n[isr] %s vector=%u err=0x%08x eip=0x%08x\n",
        exception_name(r->int_no),
        r->int_no,
        r->err_code,
        r->eip
    );

    /*
     * 对 breakpoint（int3）我们允许返回，这样可以用它做"自检"。
     * 其他异常先简单 halt，避免继续运行导致更难排查。
     */
    if (r->int_no == 3) {
        return;
    }

    /* 进入这里说明发生了"不可恢复"的 CPU 异常。 */
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

    /*
     * 硬件 IRQ（来自 8259 PIC）
     *
     * 约定：PIC remap 到 0x20..0x2F。
     * 这里先把 gate 装进去，真正要收到 IRQ 还需要：
     * - pic_init() 做 remap
     * - 解除对应 IRQ mask（例如键盘 IRQ1）
     * - sti 打开 IF
     */
    idt_set_gate(0x20, irq0,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x21, irq1,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x22, irq2,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x23, irq3,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x24, irq4,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x25, irq5,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x26, irq6,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x27, irq7,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x28, irq8,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x29, irq9,  GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x2A, irq10, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x2B, irq11, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x2C, irq12, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x2D, irq13, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x2E, irq14, GDT_KERNEL_CODE_SEL, INT_GATE);
    idt_set_gate(0x2F, irq15, GDT_KERNEL_CODE_SEL, INT_GATE);

    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base = (uint32_t)(uintptr_t)&idt[0];

    /*
     * lidt 生效后，CPU 就会使用我们的 IDT。
     * 注意：这一步不会自动开中断（IF），是否 sti 由你自己决定。
     */
    lidt(&idtr);
}
