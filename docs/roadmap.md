# SZY-KERNEL 项目愿景与路线图

## 1. 终极目标

构建一个 **x86 微内核操作系统**，最终能：

- 自举移植 **GCC** 编译器（能在自己的 OS 上编译 C 代码）
- 运行 **Vim** 文本编辑器
- 支持 **TCP/IP 网络访问**（至少能 ping、做简单的 HTTP 请求）

### 设计原则

- **自研优先**：内存管理、分页、进程调度、系统调用全部自己实现，不用现成库
- **POSIX 子集兼容**：不追求完整 POSIX，但要实现足够的子集让 GCC/Vim 的移植可行
- **微内核架构**：内核只做最核心的事（内存、调度、IPC），驱动和文件系统尽量在用户态

---

## 2. 阶段化路线图

### 里程碑 A：裸机基础设施 ✅ 已完成

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| A-1 | 引导与串口 | Multiboot2 引导、C 入口、COM1 串口输出 | ✅ |
| A-2 | CPU 基础 | GDT（平坦段）、IDT（异常 0-31）、int3 自检 | ✅ |
| A-3 | 中断与输入 | 8259 PIC、IRQ0-15、PS/2 键盘驱动、环形缓冲 | ✅ |
| A-4 | 交互式 Shell | 行编辑、命令注册表、console 输入聚合、内置命令 | ✅ |

### 里程碑 B：内存管理 ✅ 已完成

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| B-1 | PMM（bitmap） | Multiboot2 mmap 解析多 region、bitmap 分配器、`pmm_alloc/free_page`、可插拔 `pmm_ops_t` 接口 | ✅ |
| B-2 | PMM（buddy） | buddy system 物理内存分配器后端，order 0..10 的 2^n 页分裂/合并 | ✅ |
| B-3 | VMM（虚拟内存） | 两级页表、identity mapping 0-16MiB、CR3/CR0.PG 开启分页、Page Fault handler | ✅ |

### 里程碑 C：高级内存 ✅ 已完成

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| C-1 | High-Half Kernel | 内核映射到 `0xC0000000+`，拆除低地址 identity mapping | ✅ |
| C-2 | 内核堆（first-fit） | `kmalloc(size)` / `kfree(ptr)`，空闲链表分配器、可插拔 `heap_ops_t` 接口 | ✅ |
| C-3 | 内核堆（slab） | slab 分配器堆后端，7 级 cache 32B~2048B，bitmap 管理空闲 slot | ✅ |
| C-4 | VMA（sorted-array） | `vma_ops_t` 可插拔后端接口 + dispatch 层、sorted-array 后端（256 条目、二分查找）、内核地址空间 VMA 跟踪、Page Fault 按 VMA 分发权限检查、`vma` shell 命令 | ✅ |
| C-5 | VMA（红黑树） | 红黑树后端（`vma_rbtree.c`），静态节点池，$O(\log n)$ 全操作，生产级实现 | |
| C-6 | 内存子系统集成测试 | 切换 buddy + slab + VMA 默认组合，全面回归测试（pmm/heap/vmm/vma 命令），确认无碎片/泄漏 | |

### 里程碑 D：进程与用户态 ← **下一步**

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| D-1 | TSS 与 Ring 3 | Task State Segment、用户态栈、`iret` 跳转到 Ring 3 | |
| D-2 | ELF 加载器 | 解析 ELF32 文件头、加载 `.text`/`.data` 到用户地址空间，VMA 记录各段映射 | |
| D-3 | 系统调用 (Syscall) | `int 0x80` 或 `sysenter`，实现 `write`/`exit`/`brk` 等基础 syscall | |
| D-4 | 进程管理 | PCB（进程控制块）、per-process VMA 树与页表、`fork`（COW + VMA 复制）/`exec`/`waitpid` | |
| D-5 | 调度器 | 时间片轮转（Round-Robin）、上下文切换（寄存器保存/恢复 + CR3 切换） | |

### 里程碑 E：文件系统

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| E-1 | VFS 层 | 虚拟文件系统接口：`open`/`read`/`write`/`close`/`stat`/`readdir` | |
| E-2 | initrd / ramfs | 内存文件系统（把初始文件打包进内核镜像，让 ELF 加载器能读文件） | |
| E-3 | FAT32 或 ext2 | 磁盘文件系统（只读优先，后加写入），ATA/AHCI 磁盘驱动 | |
| E-4 | 设备文件 | `/dev/null`、`/dev/serial`、`/dev/console` 等字符设备 | |

### 里程碑 F：POSIX 兼容层

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| F-1 | 信号 (Signals) | `SIGINT`/`SIGTERM`/`SIGKILL`、信号投递与默认处理 | |
| F-2 | 管道与重定向 | `pipe()`、`dup2()`，Shell 支持 `|` 和 `>` | |
| F-3 | mmap | 用户态 `mmap`/`munmap`（匿名映射 + 文件映射），基于 VMA 管理 | |
| F-4 | 用户态 libc | 移植 musl-libc（或精简子集），提供 `printf`/`malloc`/`fopen` 等 | |
| F-5 | 多用户 Shell | 用户态 `/bin/sh`（非内核内置），支持环境变量、PATH 查找、job control | |

### 里程碑 G：移植 GCC 与 Vim

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| G-1 | 交叉编译工具链 | 在宿主机构建 `i686-szy-gcc` 交叉编译器（GCC + binutils） | |
| G-2 | 移植 GCC | 让 GCC 能在 SZY-KERNEL 上编译并运行简单 C 程序 | |
| G-3 | 终端子系统 | termios、伪终端（PTY）、VT100 转义序列解析 | |
| G-4 | 移植 Vim | 让 Vim 能在 SZY-KERNEL 上运行（依赖 termios + libc + 文件系统） | |

### 里程碑 H：网络

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| H-1 | 网卡驱动 | Intel E1000 (QEMU 默认网卡) 驱动：收发以太网帧 | |
| H-2 | TCP/IP 协议栈 | ARP、IP、ICMP (ping)、UDP、TCP（可考虑移植 lwIP 或自写精简版） | |
| H-3 | Socket API | `socket`/`bind`/`listen`/`accept`/`connect`/`send`/`recv` | |
| H-4 | 网络工具 | 用户态 `ping`、简易 HTTP client | |

---

## 3. 已完成 Stage 总结

### A-1~A-4: 裸机基础 → 交互式 Shell
- Multiboot2 引导、GDT/IDT、8259 PIC、PS/2 键盘
- 串口输出 printk、console 输入聚合
- 命令注册表 shell（cls/cmds/shutdown/mmap）

### B-1: PMM（bitmap）
- Multiboot2 mmap 解析多 region
- bitmap 物理页分配器、`pmm_alloc_page`/`pmm_free_page`
- 可插拔 `pmm_ops_t` 接口
- shell 命令：`free`、`pmm`

### B-2: PMM（buddy）
- buddy system 物理内存分配器后端
- order 0..10 的 2^n 页分裂/合并

### B-3: VMM (Identity Mapping)
- x86 两级页表（Page Directory + Page Table）
- Identity mapping 0-16MiB，开启 CR0.PG
- paging_flush.asm（CR3 加载、分页开启、invlpg）
- Page Fault handler（读 CR2、解码 error code）
- shell 命令：`vmm`（state/lookup/pd/pt/map/unmap/fault）

### C-1: High-Half Kernel
- boot.asm 用 4MiB PSE 页建立临时 identity + high-half 双重映射
- vmm_init() 用 4KiB 页表替换 PSE 映射
- 所有物理地址访问改为 PHYS_TO_VIRT()（PMM bitmap、Multiboot2 info、selftest）
- vmm_unmap_identity() 拆除 PD[0..767]，低地址空间留给用户态
- g_mb2_info 在 vmm_init() 后转为虚拟地址
