#pragma once

/*
 * syscall_int80.h — int 0x80 系统调用后端声明（D-3）
 *
 * [WHY] 遵循项目的可插拔后端模式：
 *   头文件只暴露一个 get_ops() 函数，调用方无需了解后端内部实现。
 *   kmain.c 通过 KCONFIG_SYSCALL_BACKEND 选择后端，调用
 *   syscall_register_backend(syscall_int80_get_ops()) 注册即可。
 */

#include "syscall.h"

/*
 * syscall_int80_get_ops — 返回 int 0x80 后端的操作表指针
 *
 * 返回的 syscall_ops_t 包含：
 *   .name = "int0x80"
 *   .init = int80_init  (在 IDT[0x80] 安装 DPL=3 的中断门)
 */
const syscall_ops_t *syscall_int80_get_ops(void);
