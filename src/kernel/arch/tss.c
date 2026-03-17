/*
 * tss.c — Task State Segment 初始化
 *
 * [WHY] TSS 是 x86 保护模式下特权级切换的关键数据结构。
 *   当 CPU 从 Ring 3 切换到 Ring 0（中断/异常/syscall）时，
 *   它从 TSS 的 SS0:ESP0 字段获取内核栈的位置。
 *
 * 本文件做 3 件事：
 *   1. 初始化 TSS 结构体（设置 SS0 和 ESP0）
 *   2. 在 GDT 中写入 TSS 描述符
 *   3. 用 LTR 指令加载 TSS 到 TR 寄存器
 */

#include "tss.h"
#include "gdt.h"
#include "printk.h"

/*
 * 内核栈顶地址（在 boot.asm 中定义并 global 导出）。
 *
 * [WHY] TSS.ESP0 需要指向内核栈顶，这样 Ring 3→Ring 0 切换时
 *   CPU 能把旧的 SS/ESP/EFLAGS/CS/EIP 压入内核栈。
 *   stack_top 是 boot.asm 的 .bss 段中分配的 16KiB 栈的顶部。
 *
 * [NOTE] 这是链接器符号，取地址用 &stack_top。
 */
extern uint32_t stack_top;

/*
 * 全局 TSS 实例
 *
 * [WHY] x86 的 TR 寄存器指向 GDT 中的 TSS 描述符，而描述符
 *   中的 base 字段指向这个结构体。整个系统只需要一个 TSS。
 *   （将来做进程切换时，只需要更新 TSS.ESP0，不需要多个 TSS。）
 */
static tss_t g_tss;

/*
 * kmemset — 简易内存填充（避免依赖 libc）
 */
static void kmemset(void* dst, uint8_t val, uint32_t size) {
    uint8_t* p = (uint8_t*)dst;
    for (uint32_t i = 0; i < size; i++) {
        p[i] = val;
    }
}

/*
 * ltr — 加载 TSS 到任务寄存器
 *
 * [WHY] LTR 指令告诉 CPU："TSS 描述符在 GDT 中这个偏移位置"。
 *   之后 CPU 发生特权级切换时就会到这个 TSS 读 SS0:ESP0。
 *
 * [CPU STATE] 执行后：
 *   TR 寄存器 = sel（指向 GDT[5]）
 *   GDT 中对应 TSS 描述符的 type 字段自动从 0x89(Available)
 *   变为 0x8B(Busy)——CPU 自动标记。
 */
static inline void ltr(uint16_t sel) {
    __asm__ volatile("ltr %0" : : "r"(sel));
}

void tss_init(void) {
    /* 1. 清零整个 TSS（104 字节） */
    kmemset(&g_tss, 0, sizeof(g_tss));

    /*
     * 2. 设置 SS0（Ring 0 栈段选择子）
     *
     * [WHY] CPU 从 Ring 3 切换到 Ring 0 时，会把 SS 设为 TSS.SS0。
     *   这必须是内核数据段选择子（0x10），因为内核栈属于内核数据段。
     */
    g_tss.ss0 = GDT_KERNEL_DATA_SEL;

    /*
     * 3. 设置 ESP0（Ring 0 栈顶指针）
     *
     * [WHY] CPU 从 Ring 3 切换到 Ring 0 时，会把 ESP 设为 TSS.ESP0。
     *   这里设为 boot.asm 中分配的 16KiB 内核栈的栈顶。
     *
     * [CRITICAL] ESP0 必须指向栈顶（高地址端），因为 x86 栈向低地址增长。
     */
    g_tss.esp0 = (uint32_t)(uintptr_t)&stack_top;

    /*
     * 4. 设置 iomap_base（I/O 权限位图偏移）
     *
     * [WHY] 设为 sizeof(tss_t) 表示"没有 I/O 权限位图"。
     *   这意味着 Ring 3 代码执行 IN/OUT 指令时会触发 #GP。
     *   这正是我们想要的：用户态不能直接访问硬件端口。
     */
    g_tss.iomap_base = sizeof(tss_t);

    /*
     * 5. 在 GDT[5] 写入 TSS 描述符
     *
     * [WHY] CPU 通过 GDT 找到 TSS 的位置。TSS 描述符告诉 CPU：
     *   - TSS 在内存中的线性地址（base）
     *   - TSS 的大小（limit = sizeof(tss_t) - 1 = 103）
     */
    uint32_t base  = (uint32_t)(uintptr_t)&g_tss;
    uint32_t limit = sizeof(tss_t) - 1;
    gdt_set_tss_entry(base, limit);

    /*
     * 6. 加载 TSS 到任务寄存器
     *
     * [CPU STATE]
     *   TR ← 0x28 (GDT_TSS_SEL)
     *   CPU 将读取 GDT[5]，验证其为有效 TSS 描述符，
     *   缓存 TSS 的基址和 limit，并把 type 从 0x89 改为 0x8B(Busy)。
     */
    ltr(GDT_TSS_SEL);

    printk("[init] tss ok (esp0=0x%08x, ss0=0x%04x)\n",
           g_tss.esp0, g_tss.ss0);
}

void tss_set_kernel_stack(uint32_t esp0) {
    g_tss.esp0 = esp0;
}
