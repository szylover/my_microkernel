#include <stddef.h>

#include "syscall.h"
#include "printk.h"

/*
 * syscall.c — 系统调用 dispatch 层（可插拔后端转发）
 *
 * [WHY] 和 pmm.c / vma.c 一样的"前端 dispatch"模式：
 *   所有后端（int 0x80 / sysenter）最终都调 syscall_dispatch()，
 *   由本文件查 syscall_table 并转发到具体 handler。
 *
 * 架构：
 *
 *   用户态 → int 0x80 (或 sysenter) → 汇编 stub → syscall_dispatch()
 *                                                       │
 *                                                       ▼
 *                                              syscall_table[nr]()
 *                                            (sys_write / sys_exit / ...)
 *
 * 后端切换只需在 syscall_init() 前调用 syscall_register_backend()。
 */

/* ============================================================================
 * syscall table — 系统调用号 → handler 映射
 * ============================================================================
 *
 * [WHY] 用静态数组而非链表/哈希表：
 *   - 系统调用号是小整数（通常 < 256），数组索引 O(1)
 *   - 无动态内存分配，启动早期即可使用
 *   - 未注册的 slot 为 NULL，dispatch 时检测即可
 */

static syscall_handler_t syscall_table[SYSCALL_MAX];

/* 当前注册的后端 */
static const syscall_ops_t* g_syscall_ops = NULL;
static int g_syscall_ready = 0;

/* ============================================================================
 * 内置 syscall 占位实现（stub）
 * ============================================================================
 *
 * [WHY] D-2 阶段只搭框架，不实现具体功能。
 *   这些 stub 打印调用信息帮助验证 dispatch 路径正确。
 *   D-3 阶段（int 0x80 后端）会替换为真正的实现。
 */

/*
 * sys_exit — 终止当前进程
 *
 * arg1 (EBX) = exit status
 *
 * [WHY] 当前没有进程管理，exit 只能 halt。
 *   将来 D-7/D-8（进程管理）会做 ZOMBIE 状态转换和资源回收。
 */
static int32_t sys_exit(const syscall_regs_t* regs) {
    printk("[syscall] exit(status=%d)\n", (int)regs->arg1);

    /* 当前无进程管理，直接 halt */
    for (;;) {
        __asm__ volatile("hlt");
    }

    /* unreachable */
    return 0;
}

/*
 * sys_write — 写数据到文件描述符
 *
 * arg1 (EBX) = fd        文件描述符（当前只支持 1=stdout, 2=stderr）
 * arg2 (ECX) = buf       用户态缓冲区地址
 * arg3 (EDX) = count     字节数
 *
 * 返回值 = 实际写入字节数，-1=错误
 *
 * [WHY] 当前还没有 VFS，write 直接输出到串口/console。
 *   将来 E-1/E-2（VFS + ramfs）会走 fd → inode → 文件系统后端。
 */
static int32_t sys_write(const syscall_regs_t* regs) {
    uint32_t fd    = regs->arg1;
    uint32_t buf   = regs->arg2;
    uint32_t count = regs->arg3;

    /* 当前只接受 stdout(1) 和 stderr(2) */
    if (fd != 1 && fd != 2) {
        return -1;
    }

    /*
     * [WHY] 安全检查：buf 必须来自用户空间。
     *   内核空间起始于 KERNEL_VIRT_OFFSET (0xC0000000)，
     *   用户态缓冲区地址应低于此。防止用户态伪造地址读内核内存。
     *
     * TODO: 更精细的检查——验证 [buf, buf+count) 全在用户 VMA 中。
     */
    if (buf >= 0xC0000000u) {
        printk("[syscall] write: bad user pointer 0x%08x\n", buf);
        return -1;
    }

    /* 上限截断，防止恶意大 count 导致长时间循环 */
    if (count > 4096) {
        count = 4096;
    }

    const char* p = (const char*)(uintptr_t)buf;
    for (uint32_t i = 0; i < count; i++) {
        printk("%c", p[i]);
    }

    return (int32_t)count;
}

/*
 * sys_brk — 调整进程数据段末尾（堆管理）
 *
 * arg1 (EBX) = new_brk  新的堆末尾地址（0 = 查询当前 brk）
 *
 * 返回值 = 当前 brk 地址
 *
 * [WHY] 当前没有用户态进程地址空间，brk 只返回占位值。
 *   将来 F-6（mmap）会和 VMA 集成实现真正的堆扩展。
 */
static int32_t sys_brk(const syscall_regs_t* regs) {
    (void)regs;

    /* 占位：返回一个固定的用户态堆起始地址 */
    printk("[syscall] brk(0x%08x) — stub, returning 0x00400000\n", regs->arg1);
    return 0x00400000;
}

/* ============================================================================
 * 后端注册
 * ============================================================================ */

void syscall_register_backend(const syscall_ops_t* ops) {
    g_syscall_ops = ops;
}

const char* syscall_backend_name(void) {
    return g_syscall_ops ? g_syscall_ops->name : NULL;
}

int syscall_is_ready(void) {
    return g_syscall_ready;
}

/* ============================================================================
 * syscall_register — 注册单个 syscall handler
 * ============================================================================ */

int syscall_register(uint32_t nr, syscall_handler_t handler) {
    if (nr == 0 || nr >= SYSCALL_MAX) {
        printk("[syscall] register: invalid nr=%u\n", nr);
        return -1;
    }
    syscall_table[nr] = handler;
    return 0;
}

/* ============================================================================
 * syscall_dispatch — 系统调用分发
 * ============================================================================
 *
 * [WHY] 后端的汇编 stub 从寄存器提取参数后调用此函数。
 *   查表 → 调用 handler → 返回值由后端放回用户态 EAX。
 *
 *   如果 nr 无效或未注册，返回 -ENOSYS (-38)。
 *   这和 Linux 行为一致：未实现的系统调用返回 -ENOSYS。
 */

#define ENOSYS 38

int32_t syscall_dispatch(const syscall_regs_t* regs) {
    if (!regs) {
        return -ENOSYS;
    }

    uint32_t nr = regs->nr;

    if (nr >= SYSCALL_MAX || !syscall_table[nr]) {
        printk("[syscall] unknown syscall nr=%u\n", nr);
        return -ENOSYS;
    }

    return syscall_table[nr](regs);
}

/* ============================================================================
 * syscall_init — 初始化系统调用子系统
 * ============================================================================ */

void syscall_init(void) {
    /* 1. 清空 syscall table */
    for (unsigned i = 0; i < SYSCALL_MAX; i++) {
        syscall_table[i] = NULL;
    }

    /* 2. 注册内置 syscall handler */
    syscall_register(SYS_EXIT,  sys_exit);
    syscall_register(SYS_WRITE, sys_write);
    syscall_register(SYS_BRK,   sys_brk);

    printk("[syscall] table: %u handlers registered "
           "(exit=%u, write=%u, brk=%u)\n",
           3, SYS_EXIT, SYS_WRITE, SYS_BRK);

    /* 3. 初始化后端（安装 IDT gate 或配置 MSR） */
    if (g_syscall_ops) {
        printk("[syscall] backend: %s\n", g_syscall_ops->name);
        g_syscall_ops->init();
    } else {
        printk("[syscall] no backend registered (dispatch only, "
               "no user-mode entry point)\n");
    }

    g_syscall_ready = 1;
    printk("[syscall] init ok\n");
}
