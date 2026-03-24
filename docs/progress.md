# Progress Board

> 粒度介于 roadmap（阶段级）和 changelog（文件级）之间的实时进度看板。
> 由 @Progress agent 维护。

## Current Focus

> **Stage D-3: 系统调用 int 0x80 后端**

## Active Stage Breakdown

| # | Task | Status | Notes |
|---|------|--------|-------|
| 1 | Design Spec | ✅ | `docs/specs/D-3-int0x80.md` |
| 2 | 公开 `regs_t` + `idt_set_gate()` (idt.h/idt.c) | ⬜ | |
| 3 | 汇编 stub `syscall_stub.asm` | ⬜ | |
| 4 | C 后端 `syscall_int0x80.c` | ⬜ | |
| 5 | 用户态封装宏 `syscall_user.h` | ⬜ | |
| 6 | 后端声明头 `syscall_backends.h` | ⬜ | |
| 7 | 用户态测试代码 `user_test_code.asm` | ⬜ | |
| 8 | `cmd_ring3.c` 新增 `ring3 syscall` 子命令 | ⬜ | |
| 9 | `kmain.c` 注册 int0x80 后端 | ⬜ | |
| 10 | 构建通过 (`make iso`) | ⬜ | |
| 11 | 端到端验证 (`ring3 syscall` → 串口输出) | ⬜ | |
| 12 | 回归测试 (pmm/heap/vma/ring3 panic) | ⬜ | |
| 13 | Book ch13 int0x80 sections | ⬜ | |
| 14 | Ship (changelog + roadmap ✅ + PR) | ⬜ | |

## Completed Stages

| Stage | Date | Key Artifact |
|-------|------|-------------|
| D-2 | 2026-03-17 | `syscall_ops_t` 接口 + dispatch 层 + `syscall_table[256]` |
| D-1 | 2026-03-17 | TSS + Ring 3 + `jump_to_ring3()` + GDT 扩展 |
| C-6 | 2026-03-16 | VMA 三后端集成测试通过 |
| C-5 | 2026-03-16 | VMA 红黑树 + Maple Tree 后端 |
| C-4 | 2026-03-16 | VMA sorted-array 后端 + `vma_ops_t` 接口 |
| C-3 | 2026-03-15 | Slab 分配器堆后端 |
| C-2 | 2026-03-15 | `kmalloc`/`kfree` first-fit + `heap_ops_t` 接口 |
| C-1 | 2026-03-15 | High-Half Kernel `0xC0000000+` |
| B-3 | 2026-03-14 | VMM 两级页表 + 分页开启 |
| B-2 | 2026-03-14 | PMM buddy system 后端 |
| B-1 | 2026-03-14 | PMM bitmap 后端 + `pmm_ops_t` 接口 |
| A-4 | 2026-03-13 | 交互式 Shell |
| A-3 | 2026-03-13 | 8259 PIC + PS/2 键盘 |
| A-2 | 2026-03-13 | GDT + IDT |
| A-1 | 2026-03-13 | Multiboot2 引导 + 串口 |

## Blocked / Parking Lot

_(无)_
