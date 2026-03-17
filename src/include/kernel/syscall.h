#pragma once

/*
 * syscall.h — 系统调用框架（可插拔后端接口）
 *
 * [WHY] 为什么需要系统调用？
 *   Ring 3（用户态）不能直接执行特权指令（如 IN/OUT/HLT）或访问内核内存。
 *   系统调用是用户态进入内核的唯一受控入口：
 *     用户态设置参数 → 触发特权级切换 → 内核执行服务 → 返回结果
 *
 * 架构（和 pmm_ops_t / vma_ops_t 同一套可插拔模式）：
 *
 *   用户态程序
 *     │  (int 0x80 / sysenter / ...)
 *     ▼
 *   syscall_ops_t 后端（汇编 stub → 切换到 Ring 0）
 *     │
 *     ▼
 *   syscall_dispatch()（本文件定义的公共 dispatch 层）
 *     │  查 syscall_table[nr]
 *     ▼
 *   具体 syscall handler（sys_write / sys_exit / sys_brk / ...）
 *
 * 调用约定（和 Linux i386 ABI 一致）：
 *
 *   EAX = 系统调用号 (syscall number)
 *   EBX = 参数 1
 *   ECX = 参数 2
 *   EDX = 参数 3
 *   ESI = 参数 4
 *   EDI = 参数 5
 *   返回值通过 EAX 传回用户态
 *
 * [WHY] 为什么用寄存器而不是栈传参？
 *   int 0x80 触发特权级切换时，CPU 会切换到内核栈（TSS.ESP0）。
 *   此时用户栈不可信（内核不应盲目访问用户栈指针）。
 *   寄存器在特权级切换前后由 CPU 保持不变，是最安全的传参方式。
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 系统调用号定义
 * ============================================================================
 *
 * [WHY] 统一编号，用户态和内核共用同一套定义。
 *   编号参考 Linux i386 但不强求完全一致——我们只实现需要的子集。
 *   预留 0 号不使用（方便检测 EAX 未初始化的 bug）。
 */

#define SYS_EXIT    1       /* 终止当前进程 */
#define SYS_WRITE   4       /* 写数据到文件描述符 */
#define SYS_BRK     45      /* 调整进程数据段末尾（堆管理） */

#define SYSCALL_MAX 256     /* syscall table 容量上限 */

/* ============================================================================
 * syscall_regs_t — 系统调用参数快照
 * ============================================================================
 *
 * [WHY] 后端（int 0x80 / sysenter）的汇编 stub 从寄存器/栈中提取参数，
 *   打包成此结构体传给 syscall_dispatch()。
 *   这样 dispatch 层和具体 handler 都不依赖特定后端的寄存器布局。
 */

typedef struct syscall_regs {
    uint32_t nr;        /* EAX: 系统调用号 */
    uint32_t arg1;      /* EBX: 参数 1 */
    uint32_t arg2;      /* ECX: 参数 2 */
    uint32_t arg3;      /* EDX: 参数 3 */
    uint32_t arg4;      /* ESI: 参数 4 */
    uint32_t arg5;      /* EDI: 参数 5 */
} syscall_regs_t;

/* ============================================================================
 * syscall handler 函数签名
 * ============================================================================
 *
 * 每个具体 syscall（如 sys_write）实现为此签名的函数，
 * 然后注册到 syscall_table[nr] 中。
 *
 * @param regs  参数快照（nr + arg1..arg5）
 * @return      返回值（放入用户态 EAX）
 */

typedef int32_t (*syscall_handler_t)(const syscall_regs_t* regs);

/* ============================================================================
 * syscall_ops_t — 后端操作表（函数指针）
 * ============================================================================
 *
 * [WHY] 不同的系统调用入口机制：
 *   - int 0x80：传统软中断方式，简单但较慢（完整中断/异常流程）
 *   - sysenter/sysexit：Intel 快速系统调用（MSR 配置，跳过 IDT 查表）
 *
 *   将入口机制抽象为后端，内核核心代码（dispatch + handler）不变。
 */

typedef struct syscall_ops {
    const char* name;       /* 后端名称（如 "int0x80", "sysenter"） */

    /*
     * init — 初始化后端
     *
     * [WHY] 后端在此设置自己的入口点：
     *   - int 0x80 后端：在 IDT[0x80] 安装 DPL=3 的中断门
     *   - sysenter 后端：配置 IA32_SYSENTER_CS/EIP/ESP MSR
     */
    void (*init)(void);
} syscall_ops_t;

/* ============================================================================
 * 后端注册（在 syscall_init 之前调用）
 * ============================================================================ */

void syscall_register_backend(const syscall_ops_t* ops);

/* ============================================================================
 * 公开 API
 * ============================================================================ */

/*
 * syscall_init — 初始化系统调用子系统
 *
 * 1. 清空 syscall_table
 * 2. 注册内置的 syscall handler（sys_write / sys_exit / sys_brk）
 * 3. 调用后端 init()（安装 IDT gate 或配置 MSR）
 */
void syscall_init(void);

/*
 * syscall_dispatch — 系统调用分发（由后端的汇编 stub 调用）
 *
 * [WHY] 后端只负责"特权级切换 + 提取参数"，转发到此函数。
 *   此函数查 syscall_table[nr] 并调用对应 handler。
 *
 * @param regs  由后端汇编 stub 填充的参数快照
 * @return      handler 返回值（后端将其放回用户态 EAX）
 */
int32_t syscall_dispatch(const syscall_regs_t* regs);

/*
 * syscall_register — 注册/替换一个 syscall handler
 *
 * @param nr       系统调用号（0 < nr < SYSCALL_MAX）
 * @param handler  处理函数
 * @return         0=成功, -1=nr 越界
 */
int syscall_register(uint32_t nr, syscall_handler_t handler);

/* 返回当前后端名称，NULL=未注册 */
const char* syscall_backend_name(void);

/* 系统调用子系统是否已初始化 */
int syscall_is_ready(void);

#ifdef __cplusplus
}
#endif
