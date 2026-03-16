# SZY-KERNEL

一个从零实现的 **x86 微内核操作系统**，目标是最终能自举编译 GCC、运行 Vim、支持 TCP/IP 网络。

当前处于 **里程碑 C（高级内存管理）** 阶段——内核已运行在高半区 `0xC0000000+`，拥有可插拔的物理 / 虚拟内存管理、内核堆分配器（first-fit / slab 双后端）、VMA 虚拟内存区域跟踪，以及 9 条内置命令的交互式 Shell。

## 已完成的功能

| 里程碑 | Stage | 内容 |
|--------|-------|------|
| A 裸机基础 ✅ | 1-4 | Multiboot2 引导、GDT/IDT、8259 PIC、PS/2 键盘、串口输出、交互式 Shell |
| B 内存管理 ✅ | 5-6 | PMM（bitmap + buddy 双后端）、VMM（两级页表、identity mapping、Page Fault） |
| C 高级内存 🚧 | 7-9 | High-Half Kernel (`0xC0000000+`)、kmalloc（first-fit + slab 双后端）、VMA 可插拔接口 |

完整路线图：[docs/roadmap.md](docs/roadmap.md)

## Shell 命令

```
cls        清屏                    free       物理内存用量
cmds       列出所有命令             pmm        PMM 调试 (state/alloc/free/dump)
shutdown   QEMU/Bochs 关机         vmm        VMM 调试 (state/lookup/pd/pt/map/unmap)
mmap       Multiboot2 内存映射      heap       内核堆调试 (status/alloc/free/test)
vma        VMA 调试 (list/find/count/test)
```

## 快速开始

```bash
# 安装依赖 (Debian/Ubuntu/WSL)
sudo apt install -y nasm binutils grub-pc-bin xorriso qemu-system-x86 gcc-multilib

# 构建 & 运行
make iso && make run

# 调试模式 (QEMU + GDB)
make DEBUG=1 iso && ./scripts/run_qemu_gdb.sh
```

## 项目结构

```
src/
├── boot/           Multiboot2 引导 & 链接脚本
├── include/
│   ├── arch/       x86 体系结构 (gdt/idt/irq/pic/io)
│   ├── drivers/    设备驱动 (keyboard/serial)
│   └── kernel/     内核通用 (pmm/vmm/vma/kmalloc/shell/printk/cmd/kconfig/…)
└── kernel/
    ├── arch/       CPU 初始化 & 汇编 stubs
    ├── cmds/       Shell 命令模块 (9 条命令)
    ├── core/       内核入口和基础设施 (kmain/shell/printk/console)
    ├── drivers/    设备驱动实现
    └── mm/         内存管理 (pmm/pmm_bitmap/pmm_buddy/vmm/vma/kmalloc/heap_first_fit/heap_slab/mmap)
book/               配套教程书籍 (LaTeX, 18 章)
docs/               项目文档
scripts/            构建/调试/测试脚本
```

## 文档

| 文档 | 内容 |
|------|------|
| [docs/howto.md](docs/howto.md) | 构建环境、依赖安装、QEMU 启动、常见问题 |
| [docs/debug.md](docs/debug.md) | QEMU + GDB 调试技巧、VS Code 集成 |
| [docs/shell.md](docs/shell.md) | Shell 设计、命令列表、扩展方法 |
| [docs/roadmap.md](docs/roadmap.md) | 项目愿景、阶段化路线图、已完成进度 |
| [docs/changelog.md](docs/changelog.md) | 每日变更记录 |
| [docs/memory-commands.md](docs/memory-commands.md) | 内存调试命令速查 |

## 配套书籍

本项目配有一本 LaTeX 编写的教程书籍 **《自己动手写操作系统：从零构建 x86 微内核》**，位于 `book/` 目录，共 18 章 + 2 个附录，覆盖从裸机引导到文件系统的完整路径。

| 部分 | 章节 | 内容 |
|------|------|------|
| 准备工作 | Ch 0-1 | 前言、开发环境搭建 |
| 裸机基础设施 | Ch 2-6 | Multiboot2 引导、GDT、IDT、IRQ/键盘、Shell |
| 内存管理 | Ch 7-11 | PMM、VMM、High-Half Kernel、kmalloc、VMA |
| 进程与用户态 | Ch 12-16 | TSS/Ring3、ELF Loader、系统调用、进程、调度器 |
| 文件系统 | Ch 17-18 | VFS、RamFS |
| 附录 | A-B | 工具速查、x86 硬件参考 |

### 构建书籍 PDF

```bash
# 安装 LaTeX 依赖
sudo apt install texlive-xetex texlive-lang-chinese texlive-latex-extra \
     texlive-fonts-recommended texlive-science latexmk

# 编译 PDF（两遍 xelatex，生成目录和交叉引用）
cd book && make

# 快速预览（只跑一遍，目录可能为空）
cd book && make once

# 持续监听自动编译
cd book && make watch
```

## 技术亮点

- **可插拔后端模式**：PMM（bitmap / buddy）、堆（first-fit / slab）、VMA 均通过 `ops_t` 函数指针表实现后端热切换，编译期由 `kconfig.h` 选择
- **High-Half Kernel**：内核运行在 `0xC0000000+`，低地址空间预留给未来用户态进程
- **学习友好**：所有底层代码附带详细 `[WHY]` / `[CPU STATE]` / `[BITFIELDS]` 注释，配有 18 章 LaTeX 教程书籍
