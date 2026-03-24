# Agent 工作流

本项目使用 3 个 Agent + 1 个 Prompt 协作：

| 角色 | 文件 | 职责 | 工具 |
|------|------|------|------|
| **@Architect** | `.github/agents/architect.agent.md` | 设计接口、输出 Design Spec（只读） | read, search |
| **@Kernel** | `.github/agents/kernel.agent.md` | 实现 `src/` 下的 C/ASM 代码 | read, edit, search, execute |
| **@Author** | `.github/agents/author.agent.md` | 写 `book/` 下的 LaTeX 章节和 TikZ 图 | read, edit, search, execute |
| **/ship** | `.github/prompts/ship.prompt.md` | Merge 检查清单 + Git 工作流 | read, edit, search, execute |

## 典型流程

```
用户: "实现 D-3 int 0x80 后端"
1. @Architect  → 输出 Design Spec
2. @Kernel ║ @Author  ← 并行消费同一份 spec
3. /ship  → 检查清单 → commit → push → PR
```

## 设计原则

- **概念→可插拔接口→多后端实现**：每个子系统都有 `xxx_ops_t` 函数指针表 + dispatch 层
- 详细的代码规范见 `@Kernel`，书稿规范见 `@Author`，发布检查清单见 `/ship`

---

# 项目文件结构（同步更新）

```text
my_microkernel/
├── .github/
│   ├── copilot-instructions.md   # 本文件 — Agent 工作流 + 项目结构
│   ├── agents/
│   │   ├── architect.agent.md    #   只读架构师，输出 Design Spec
│   │   ├── kernel.agent.md       #   内核开发，实现 src/ 下代码
│   │   └── author.agent.md       #   书稿作者，写 book/ 下 LaTeX
│   └── prompts/
│       └── ship.prompt.md        #   Merge 检查清单 + Git 工作流
├── Makefile                       # 薄 wrapper，转发到 src/Makefile
├── README.md
├── book/                          # LaTeX 书稿 《自己动手写操作系统》（第一本）
│   ├── Makefile                   #   xelatex 构建（make → main.pdf）
│   ├── main.tex                   #   主文件（\include 各章）
│   ├── preamble.tex               #   宏包 & 样式定义
│   ├── Makefile                   #   xelatex 构建（make → main.pdf）
│   ├── main.tex                   #   主文件（\include 各章）
│   ├── preamble.tex               #   宏包 & 样式定义
│   ├── chapters/                  #   各章包装文件 + 子目录
│   │   ├── ch00-preface.tex       #     前言（短文件，不拆分）
│   │   ├── ch01-environment.tex   #     环境搭建（短文件）
│   │   ├── ch02-boot.tex          #     → \input{ch02-boot/sec*.tex}
│   │   ├── ch02-boot/             #       per-section 子文件
│   │   ├── …                      #     ch03–ch11 同理
│   │   ├── ch12-tss-ring3.tex     #     Part 3 进程与用户态
│   │   ├── ch13-syscall.tex       #       系统调用（syscall_ops_t 可插拔）
│   │   ├── ch14-elf-loader.tex    #       ELF 加载器（loader_ops_t 可插拔）
│   │   ├── ch15-process.tex       #       进程管理（PCB/fork/exec/waitpid）
│   │   ├── ch16-scheduler.tex     #       调度器（sched_ops_t 可插拔）
│   │   ├── ch17-vfs.tex           #     Part 4 文件系统（fs_ops_t 可插拔）
│   │   ├── ch18-ramfs.tex         #       ramfs + initrd
│   │   ├── ch19-diskfs.tex        #       块设备（blkdev_ops_t）+ ext2
│   │   ├── ch20-devfs.tex         #       字符设备（chardev_ops_t）
│   │   ├── ch21-signals.tex       #     Part 5 POSIX 兼容层
│   │   ├── ch22-pipe.tex          #       管道（ipc_ops_t 可插拔）+ 重定向
│   │   ├── ch23-mmap.tex          #       mmap + VMA 集成
│   │   ├── ch24-libc.tex          #       精简 libc
│   │   ├── ch25-user-shell.tex    #       用户态 /bin/sh
│   │   ├── ch26-cross-toolchain.tex #   Part 6 移植 GCC 与 Vim
│   │   ├── ch27-terminal.tex      #       终端子系统（tty_ops_t 可插拔）
│   │   ├── ch28-port-gcc.tex      #       移植 GCC
│   │   ├── ch29-port-vim.tex      #       移植 Vim
│   │   ├── ch30-nic-driver.tex    #     Part 7 网络（netdev_ops_t 可插拔）
│   │   ├── ch31-tcpip.tex         #       TCP/IP（proto_ops_t 可插拔）
│   │   ├── ch32-socket.tex        #       Socket API（socket_ops_t 可插拔）
│   │   └── ch33-net-tools.tex     #       ping + wget
│   └── figures/                   #   图片资源
├── book2/                         # LaTeX 书稿 《如何用 AI Agent 编写自己的操作系统》（第二本）
│   ├── Makefile                   #   xelatex 构建
│   ├── main.tex                   #   主文件
│   ├── preamble.tex               #   宏包 & 样式（含 Agent 专用 TikZ 样式）
│   └── chapters/                  #   ch01–ch10 + 2 附录
├── docs/                          # 项目文档（面向人类）
│   ├── changelog.md               #   变更日志
│   ├── debug.md                   #   QEMU + GDB 调试指南
│   ├── howto.md                   #   构建 & 运行指南
│   ├── memory-commands.md         #   内存调试命令速查
│   ├── roadmap.md                 #   项目愿景与路线图
│   └── shell.md                   #   Shell 设计文档
├── scripts/                       # 构建/调试/测试脚本
└── src/
    ├── Makefile                   # 真正的构建系统
    ├── boot/                      # Multiboot2 引导 & 链接脚本
    ├── include/                   # 公共头文件
    │   ├── arch/                  #   x86 体系结构 (gdt/idt/irq/pic/io/tss)
    │   ├── drivers/               #   设备驱动 (keyboard/serial)
    │   └── kernel/                #   内核通用 (pmm/vmm/vma/kmalloc/shell/printk/cmd/kconfig/syscall/…)
    └── kernel/                    # 内核实现
        ├── arch/                  #   CPU 初始化 & 汇编 stubs (gdt/idt/irq/pic/tss)
        ├── cmds/                  #   Shell 命令模块 (cls/cmds/free/mmap/pmm/vmm/heap/vma/ring3)
        ├── core/                  #   内核入口和基础设施 (kmain/shell/printk/console)
        ├── drivers/               #   设备驱动实现
        └── mm/                    #   内存管理 (pmm/pmm_bitmap/pmm_buddy/vmm/vma/vma_sorted_array/vma_rbtree/vma_maple/kmalloc/heap_first_fit/heap_slab/mmap)
```
