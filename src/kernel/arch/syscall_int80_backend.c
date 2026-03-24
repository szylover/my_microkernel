/*
 * syscall_int80_backend.c — int 0x80 系统调用后端实现（D-3）
 *
 * ============================================================================
 * [WHY] 为什么选择 int 0x80？
 * ============================================================================
 *
 *   int 0x80 是 x86 32-bit Linux 的传统系统调用入口。
 *   原理：在 IDT[0x80] 安装 DPL=3 的中断门。
 *   用户态执行 `int 0x80` 时，CPU 会：
 *     1. 检查 CPL(3) <= DPL(3) → 允许（若 DPL=0 则引发 #GP）
 *     2. 从 TSS.SS0:ESP0 切换到内核栈
 *     3. 压入 SS/ESP/EFLAGS/CS/EIP（完整的跨特权级帧）
 *     4. 跳转到 IDT[0x80].offset（syscall_int80_entry 汇编 stub）
 *
 * ============================================================================
 * [WHY] DPL=3 vs DPL=0 的区别
 * ============================================================================
 *
 *   - DPL=0：只有内核态（CPL=0）才能用软件 int 指令触发该向量。
 *     硬件异常（CPU 内部投递，如 #GP/#PF）不受 DPL 检查约束，仍可用 DPL=0。
 *   - DPL=3：用户态（CPL=3）也能通过 `int n` 软件触发。
 *     syscall 入口必须设 DPL=3，否则用户态执行 `int 0x80` 会触发 #GP。
 *
 * ============================================================================
 * [WHY] interrupt gate vs trap gate
 * ============================================================================
 *
 *   - Interrupt gate (type=0xE)：进入 handler 时 CPU 自动清 IF（关中断）。
 *     防止 syscall 处理期间被硬件中断抢占，早期实现更安全、更简单。
 *   - Trap gate (type=0xF)：保持 IF 状态，允许中断嵌套。
 *     Linux 的 int 0x80 实际用 trap gate，以便允许调度抢占。
 *     当前阶段我们用 interrupt gate，后续需要可抢占调度时再切换。
 *
 * ============================================================================
 * [NOTE] 文件命名约定
 * ============================================================================
 *
 *   本文件命名为 syscall_int80_backend.c（而非 syscall_int80.c），
 *   以避免与 syscall_int80.asm 产生同名 .o 文件（Makefile 的 patsubst 规则
 *   会把 .c 和 .asm 都映射到同一个 .o 路径，导致重复链接）。
 *   参考项目中 tss.c + tss_flush.asm 的命名惯例。
 */

#include "syscall_int80.h"
#include "idt.h"
#include "gdt.h"
#include "printk.h"

/*
 * 汇编入口点（在 syscall_int80_entry.asm 中定义）
 *
 * [WHY] 用 extern 声明而不是函数指针，因为汇编符号在链接时解析。
 *   这里的 void (*)(void) 类型只是告诉编译器"这是一个函数地址"，
 *   idt_install_gate() 内部会把它转成 uint32_t 写入 IDT gate。
 */
extern void syscall_int80_entry(void);

/*
 * IDT type_attr 常量
 *
 * [BITFIELDS] 0xEE = 1110_1110b
 *   bit 7    (P)    = 1    → present（描述符有效）
 *   bits 6-5 (DPL)  = 11   → privilege level 3（Ring 3 可软件触发）
 *   bit 4    (S)    = 0    → 系统描述符（gate，非代码/数据段）
 *   bits 3-0 (Type) = 1110 → 32-bit interrupt gate（进入时自动清 IF）
 *
 * 对比 0x8E（内核异常/IRQ 用）：
 *   bit 7    (P)    = 1
 *   bits 6-5 (DPL)  = 00   → 只有 Ring 0 可软件触发
 *   bit 4    (S)    = 0
 *   bits 3-0 (Type) = 1110
 */
#define INT_GATE_DPL3  0xEE

/*
 * int80_init — 初始化 int 0x80 后端
 *
 * 在 IDT[0x80] 安装 DPL=3 的 32-bit interrupt gate。
 *
 * [WHY] 必须在 idt_init() 之后调用，因为 idt_init() 会清空并重新加载整个 IDT。
 *   syscall_init() → g_syscall_ops->init() → int80_init() 的调用顺序保证了这一点
 *   （idt_init() 在 kmain.c 中先于 syscall_init() 执行）。
 *
 * [CPU STATE] 调用后：
 *   IDT[0x80].offset   = &syscall_int80_entry（汇编入口地址）
 *   IDT[0x80].selector = GDT_KERNEL_CODE_SEL (0x08)
 *   IDT[0x80].type_attr= 0xEE（P=1, DPL=3, 32-bit interrupt gate）
 *
 *   此后，Ring 3 执行 `int 0x80` 时 CPU 会：
 *     1. 通过 DPL 检查（CPL=3 ≤ DPL=3）
 *     2. 切换到 TSS.SS0:ESP0（内核栈）
 *     3. 压入 SS/ESP/EFLAGS/CS/EIP
 *     4. 跳转到 syscall_int80_entry
 */
static void int80_init(void) {
    idt_install_gate(0x80,                  /* vector: int 0x80 */
                     syscall_int80_entry,   /* handler: 汇编入口 */
                     GDT_KERNEL_CODE_SEL,   /* selector: 0x08 内核代码段 */
                     INT_GATE_DPL3);        /* type_attr: DPL=3 interrupt gate */

    printk("[syscall/int0x80] IDT[0x80] installed "
           "(DPL=3, selector=0x%02x, entry=%p)\n",
           GDT_KERNEL_CODE_SEL,
           (void *)syscall_int80_entry);
}

/*
 * int80_ops — int 0x80 后端操作表
 *
 * [WHY] const static：操作表在编译期固定，不可修改，节省内存且安全。
 *   通过指针暴露给外部（syscall_int80_get_ops），
 *   外部只能读取，无法通过指针修改内容。
 */
static const syscall_ops_t int80_ops = {
    .name = "int0x80",
    .init = int80_init,
};

/*
 * syscall_int80_get_ops — 返回 int 0x80 后端的操作表指针
 *
 * [WHY] 遵循项目的"get_ops"模式（与 pmm_bitmap_get_ops / vma_sorted_array_get_ops 一致）：
 *   外部代码只持有 syscall_ops_t* 指针，不依赖具体后端实现。
 *   切换后端时只需修改 kmain.c 中的 syscall_register_backend() 调用。
 */
const syscall_ops_t *syscall_int80_get_ops(void) {
    return &int80_ops;
}
