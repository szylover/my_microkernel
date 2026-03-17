#pragma once

#include <stdint.h>

/*
 * tss.h — Task State Segment (TSS)
 *
 * ============================================================================
 * [WHY] 为什么需要 TSS？
 * ============================================================================
 *
 * 当 CPU 从 Ring 3（用户态）切换到 Ring 0（内核态）时——无论是中断、
 * 异常还是系统调用——CPU 需要知道"内核栈在哪里"。
 *
 * 答案就在 TSS 中的 SS0:ESP0 字段：
 *   SS0  = 内核数据段选择子 (0x10)
 *   ESP0 = 内核栈顶地址
 *
 * CPU 在特权级切换时会自动读取 TSS.SS0:ESP0，把栈切换到内核栈，
 * 然后将旧的 SS/ESP/EFLAGS/CS/EIP 压入内核栈。
 *
 * 经典 x86 TSS 有 104 字节，包含所有寄存器快照（用于硬件任务切换）。
 * 但现代操作系统（Linux、Windows）都用软件任务切换，**只使用 SS0:ESP0**。
 * 其他字段全部保持 0。
 *
 * ============================================================================
 * [BITFIELDS] TSS 结构体布局（Intel SDM Vol.3 Fig.7-2）
 * ============================================================================
 *
 *   Offset  Size  Field        说明
 *   ------  ----  ------------ -----------------------------------------
 *   0x00    4     prev_tss     链接到前一个 TSS（硬件任务切换用，不用）
 *   0x04    4     esp0         ★ Ring 0 栈指针
 *   0x08    4     ss0          ★ Ring 0 栈段选择子
 *   0x0C    4     esp1         Ring 1 栈指针（不用）
 *   0x10    4     ss1          Ring 1 栈段选择子（不用）
 *   0x14    4     esp2         Ring 2 栈指针（不用）
 *   0x18    4     ss2          Ring 2 栈段选择子（不用）
 *   0x1C    4     cr3          页目录基址（硬件任务切换用，不用）
 *   0x20    4     eip          （不用）
 *   0x24    4     eflags       （不用）
 *   0x28-34 16    eax-ebx      （不用）
 *   0x38-44 16    esp-edi      （不用）
 *   0x48-5C 24    es-gs        （不用）
 *   0x60    4     ldt          LDT 段选择子（不用）
 *   0x64    2     trap         调试陷阱标志（不用）
 *   0x66    2     iomap_base   I/O 权限位图偏移（设为 sizeof(tss)）
 *
 *   总计: 104 字节 (0x68)
 */
typedef struct __attribute__((packed)) {
    uint32_t prev_tss;
    uint32_t esp0;          /* ★ Ring 0 → 中断/异常/syscall 时 CPU 加载此值到 ESP */
    uint32_t ss0;           /* ★ Ring 0 → CPU 加载此值到 SS */
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} tss_t;

/*
 * tss_init — 初始化 TSS 并加载到 TR 寄存器
 *
 * 做 5 件事：
 *   1. 清零 TSS 结构体
 *   2. 设置 ss0 = KERNEL_DATA_SEG
 *   3. 设置 esp0 = 内核栈顶
 *   4. 在 GDT[5] 写入 TSS 描述符
 *   5. ltr(0x28) 加载 TSS 到任务寄存器
 *
 * [CPU STATE] 调用后：
 *   TR 寄存器指向 GDT[5] 的 TSS 描述符
 *   CPU 发生 Ring 3→Ring 0 切换时会自动读取 TSS.SS0:ESP0
 */
void tss_init(void);

/*
 * tss_set_kernel_stack — 更新 TSS 中的内核栈指针
 *
 * [WHY] 将来做进程切换时，每个进程有独立的内核栈。
 *   切换进程后需要更新 TSS.ESP0，否则下次中断会用错误的栈。
 *
 * @param esp0  新的内核栈顶地址
 */
void tss_set_kernel_stack(uint32_t esp0);

/*
 * jump_to_ring3 — 从 Ring 0 跳转到 Ring 3 执行（汇编实现）
 *
 * 构造 iret 帧并执行 iret，把 CPU 切换到用户态：
 *   push SS3     (0x23 = user data | RPL=3)
 *   push ESP3    (用户态栈顶)
 *   push EFLAGS  (IF=1 保持中断)
 *   push CS3     (0x1B = user code | RPL=3)
 *   push EIP3    (用户态入口地址)
 *   iret
 *
 * [CPU STATE] iret 执行后：
 *   CPL: 0 → 3
 *   CS: 0x08 → 0x1B, SS: 0x10 → 0x23
 *   EIP: 指向 eip 参数, ESP: 指向 esp3 参数
 *
 * 此函数不返回。
 */
void jump_to_ring3(uint32_t eip, uint32_t esp3);
