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
| C-5 | VMA（红黑树 + Maple Tree） | 红黑树后端（`vma_rbtree.c`）+ Maple Tree 后端（`vma_maple.c`），静态节点池，$O(\log n)$ 全操作，`kconfig.h` 选择 | ✅ |
| C-6 | 内存子系统集成测试 | 三后端均通过 `vma test` 全部 4 项自测，sorted-array / rbtree / maple-tree 功能验证通过 | ✅ |

### 里程碑 D：进程与用户态 ← **下一步**

> **设计原则**：沿用里程碑 B/C 的"概念→可插拔接口→多后端实现"递进模式。
> 三个可插拔接口：`syscall_ops_t`（系统调用分发）、`loader_ops_t`（可执行文件加载）、`sched_ops_t`（调度算法）。

#### D-α：特权级切换 (ch12)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| D-1 | TSS 与 Ring 3 | x86 特权级概念（CPL/DPL/RPL）、TSS 结构体、GDT 扩展（User Code 0x1B / User Data 0x23 / TSS 0x28）、`iret` 跳转 Ring 3、验证 GP Fault 回内核 | ✅ |

#### D-β：系统调用 (ch13)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| D-2 | 系统调用概念 + `syscall_ops_t` 接口 | 用户态→内核态受控入口、调用约定（EAX=nr, EBX~EDI=args）、可插拔 `syscall_ops_t`（init/entry）+ dispatch 层 + syscall table | |
| D-3 | 系统调用（int 0x80）后端 | IDT gate 0x80 DPL=3、汇编 stub、第一批 syscall：`write`/`exit`/`brk`、用户态 inline asm 封装、验证 Ring 3 → int 0x80 → write("hello") | |
| D-4 | 系统调用（sysenter）后端 *(进阶)* | MSR 配置（IA32_SYSENTER_CS/EIP/ESP）、sysenter/sysexit 快速路径、`KCONFIG_SYSCALL_BACKEND` 切换 | |

#### D-γ：可执行文件加载 (ch14)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| D-5 | ELF 概念 + `loader_ops_t` 接口 | 可执行文件格式概述、ELF32 结构详解（Ehdr/Phdr/PT_LOAD）、可插拔 `loader_ops_t`（validate/load/get_entry）+ dispatch 层 | |
| D-6 | ELF32 加载器后端 | 验证 magic+type+machine、遍历 PT_LOAD 段（分配物理页→映射→复制→VMA 注册）、入口点 `e_entry`、验证加载最小 ELF → syscall exit | |

#### D-δ：进程管理 (ch15)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| D-7 | 进程抽象 + PCB | 进程概念（地址空间 + 执行上下文 + 资源集合）、`task_struct` 数据结构、per-process 页目录与 VMA 树（内核共享/用户独立）、进程状态机（CREATED→READY→RUNNING→BLOCKED→ZOMBIE）、PID 分配 | |
| D-8 | fork / exec / waitpid | `fork`：复制 PCB + VMA + COW 页表（共享物理页、标记只读、#PF 时复制）；`exec`：清空用户空间→ELF 加载→重设入口；`exit` + `waitpid`：进程终止+资源回收+ZOMBIE 处理 | |

#### D-ε：调度器 (ch16)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| D-9 | 调度概念 + `sched_ops_t` 接口 + PIT + 上下文切换 | 协作式 vs 抢占式、时间片概念、可插拔 `sched_ops_t`（init/enqueue/dequeue/pick_next/tick）+ dispatch 层、PIT 8253 配置（IRQ0, 100Hz）、上下文切换汇编（save regs → switch CR3 → update TSS.ESP0 → restore regs → iret） | |
| D-10 | Round-Robin 调度器 | 环形就绪队列、固定时间片轮转、IRQ0 handler 调 `schedule()`、验证两进程交替输出 | |
| D-11 | 优先级调度器 *(进阶)* | 多级就绪队列、动态优先级调整（aging/boost）、`KCONFIG_SCHED_BACKEND` 切换 | |

#### D-ζ：集成测试

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| D-12 | 进程子系统集成测试 | 多进程并发（fork+exec+scheduler）、syscall 全路径验证、地址空间隔离验证、`proc test` shell 命令 | |

### 里程碑 E：文件系统

> **设计原则**：沿用"概念→可插拔接口→多后端实现"递进模式。
> 三个可插拔接口：`fs_ops_t`（文件系统后端）、`blkdev_ops_t`（块设备驱动）、`chardev_ops_t`（字符设备驱动）。

#### E-α：VFS 与内存文件系统 (ch17, ch18)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| E-1 | VFS 概念 + `fs_ops_t` 接口 | "一切皆文件"哲学、VFS 四大对象（superblock/inode/dentry/file）概念、可插拔 `fs_ops_t`（mount/open/read/write/close/stat/readdir）+ dispatch 层、per-process fd 表、fd 0/1/2 约定 | |
| E-2 | ramfs 后端 | 内存文件系统实现（链表目录树、文件内容驻内存）、实现 `fs_ops_t` 全部方法、VFS 系统调用集成（sys_open/read/write/close） | |
| E-3 | initrd 加载 | GRUB Multiboot2 module tag 解析、打包格式（CPIO/tar/自定义）、ramfs 根目录挂载、验证 `exec("/hello")` 从 initrd 加载 ELF | |

#### E-β：块设备与磁盘文件系统 (ch19)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| E-4 | 块设备概念 + `blkdev_ops_t` 接口 | 块设备 vs 字符设备、扇区/块抽象、可插拔 `blkdev_ops_t`（read_block/write_block/get_capacity）+ dispatch 层 | |
| E-5 | ATA PIO 驱动后端 | ATA PIO-mode 扇区读写（I/O ports 0x1F0–0x1F7）、28-bit LBA 寻址、IRQ14 中断驱动、`KCONFIG_BLKDEV_BACKEND` 选择 | |
| E-6 | ext2 只读后端 | ext2 磁盘布局（超级块/块组描述符/inode 表/数据块）、实现 `fs_ops_t`（只读：open/read/stat/readdir）、挂载 QEMU 磁盘镜像 | |
| E-7 | ext2 写入支持 *(进阶)* | ext2 写入路径（inode 分配/块分配/目录项插入）、bitmap 管理、fsync 策略 | |

#### E-γ：字符设备 (ch20)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| E-8 | 字符设备概念 + `chardev_ops_t` 接口 | Unix 设备模型（主设备号/次设备号）、可插拔 `chardev_ops_t`（open/read/write/ioctl）+ cdev 注册框架 | |
| E-9 | 基础字符设备 | `/dev/null`（丢弃写、读返回 EOF）、`/dev/zero`（读返回 0）、`/dev/serial`（串口映射）、`/dev/console`（键盘+屏幕） | |

#### E-ζ：集成测试

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| E-10 | 文件子系统集成测试 | fd 生命周期（open→read/write→close）、ramfs 文件 CRUD、块设备扇区读写、设备文件功能验证、`fs test` shell 命令 | |

### 里程碑 F：POSIX 兼容层

> **设计原则**：每个 POSIX 特性独立递进——先讲清概念和 POSIX 语义，再实现内核机制，最后集成验证。
> 可插拔接口：`ipc_ops_t`（进程间通信后端，pipe/shm/消息队列可扩展）。

#### F-α：信号 (ch21)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| F-1 | 信号概念 + 信号表 | 信号本质（异步进程通知）、信号生命周期（产生→挂起→投递→处理）、31 个标准信号（`SIGINT`/`SIGTERM`/`SIGKILL`/`SIGCHLD`…）、per-process pending/blocked 位图 | |
| F-2 | 信号投递 + 处理器 | 内核态→用户态信号跳板（signal trampoline）、默认动作（terminate/ignore/stop/core）、`sigaction` 系统调用注册自定义处理函数、信号屏蔽与嵌套、验证 `Ctrl+C` → `SIGINT` → 终止前台进程 | |

#### F-β：管道与 I/O 重定向 (ch22)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| F-3 | 管道概念 + `ipc_ops_t` 接口 | Unix 管道原理（匿名/命名）、IPC 分类概述、可插拔 `ipc_ops_t`（create/read/write/close/poll）+ dispatch 层 | |
| F-4 | pipe + dup2 实现 | 匿名管道后端（内核环形缓冲区、阻塞读写、EOF 检测）、`dup2()` fd 复制、Shell 集成（`|` 管道 + `>` 重定向）、验证 `ls | grep foo` 风格管道链 | |

#### F-γ：mmap (ch23)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| F-5 | mmap 概念 + VMA 集成 | mmap 用途（动态库/共享内存/大文件）、匿名映射 vs 文件映射、demand paging + COW 在 mmap 中的应用 | |
| F-6 | mmap 匿名映射后端 | `sys_mmap`/`sys_munmap` 实现（`MAP_ANONYMOUS|MAP_PRIVATE`）、VMA 动态添加/移除、#PF 按需分配零页、验证用户态 malloc 大块走 mmap | |
| F-7 | mmap 文件映射后端 *(进阶)* | `MAP_PRIVATE` 文件映射（VFS 集成、按需从文件读入页）、`MAP_SHARED` 写回策略、`KCONFIG_MMAP_FILE` 开关 | |

#### F-δ：用户态 libc (ch24)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| F-8 | libc 概念 + 选型 | libc 在用户程序与内核之间的角色、musl vs glibc vs newlib 对比、C 运行时启动流程（`_start` → `__libc_start_main` → `main`） | |
| F-9 | 精简 libc 实现 | crt0.o 启动代码、syscall wrapper 层、核心函数实现（printf/sprintf/malloc/free/fopen/fread/string.h/ctype.h）、静态链接 libc.a、验证用户态 `printf("hello %d\n", 42)` | |

#### F-ε：用户态 Shell (ch25)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| F-10 | 用户态 Shell 概念 | 用户态 shell vs 内核内置 shell 的区别、`fork+exec` 命令执行模型、进程组与会话概念、job control 基础 | |
| F-11 | `/bin/sh` 实现 | 命令行解析（tokenizer）、内置命令（cd/export/exit）、外部命令（PATH 查找+fork+exec）、环境变量继承、前台/后台进程（`&`）、`Ctrl+C`/`Ctrl+Z` 信号转发、验证替代内核 shell 启动 | |

#### F-ζ：集成测试

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| F-12 | POSIX 层集成测试 | 信号全路径（产生→投递→处理→返回）、管道+重定向组合场景、mmap 匿名/文件映射验证、libc 基础函数覆盖、用户态 shell 启动→执行多命令→退出、`posix test` shell 命令 | |

### 里程碑 G：移植 GCC 与 Vim

> **设计原则**：先搭建基础设施（交叉工具链 + 终端），再逐步满足 GCC/Vim 运行时依赖。
> 可插拔接口：`tty_ops_t`（终端行规程后端，raw/canonical/cooked 模式可切换）。

#### G-α：交叉编译工具链 (ch26)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| G-1 | 交叉编译概念 | cross-compilation 原理、GCC 构建三元组（build/host/target）、sysroot 机制、`i686-szy` 目标定义 | |
| G-2 | 构建 i686-szy-gcc | binutils 构建（as/ld/objdump）→ GCC bootstrap（C-only, --without-headers）→ 安装 libc 头文件 + libc → GCC 完整构建（libgcc + libstdc++）、sysroot 目录结构 | |

#### G-β：终端子系统 (ch27)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| G-3 | 终端概念 + `tty_ops_t` 接口 | 终端历史（物理→虚拟→伪终端）、tty/pty 区别、可插拔 `tty_ops_t`（open/read/write/ioctl/set_termios）+ dispatch 层 | |
| G-4 | termios 实现 | `struct termios` 数据结构、行规程（canonical/raw 模式）、特殊字符处理（`VEOF`/`VERASE`/`VINTR`）、回显控制、`tcgetattr`/`tcsetattr` 系统调用 | |
| G-5 | PTY + VT100 | 伪终端 master/slave 配对（`openpty`/`posix_openpt`）、VT100/ANSI 转义序列解析（光标移动 `\e[H`、颜色 `\e[31m`、清屏 `\e[2J`）、`KCONFIG_TTY_BACKEND`（basic/pty）切换 | |

#### G-γ：移植 GCC (ch28)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| G-6 | GCC 运行时依赖分析 | GCC 所需系统调用清单（fork/exec/waitpid/open/read/write/stat/mmap/brk…）、所需 libc 函数、文件系统需求（临时文件、头文件路径） | |
| G-7 | GCC 移植 + 自举验证 | 移植补丁（config.sub/config.guess 添加 `*-szy`）、Canadian cross 构建、OS 上编译 hello.c → 执行成功、自举测试（用 OS 上的 GCC 编译 GCC 自身） | |

#### G-δ：移植 Vim (ch29)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| G-8 | Vim 依赖分析 | Vim 运行时依赖（termios/PTY/libc/文件系统/信号/mmap）、最小 feature set 编译选项（`--with-features=tiny`） | |
| G-9 | Vim 移植 + 验证 | configure 补丁、编译 + 链接 szy-libc、验证完整流程（`vim test.c` → 编辑 → `:wq` → 文件保存成功） | |

### 里程碑 H：网络

> **设计原则**：网络栈自底向上递进——驱动→协议栈→Socket API→用户工具。
> 三个可插拔接口：`netdev_ops_t`（网卡驱动）、`proto_ops_t`（协议栈）、`socket_ops_t`（Socket 实现）。

#### H-α：网卡驱动 (ch30)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| H-1 | 网络概念 + `netdev_ops_t` 接口 | OSI/TCP-IP 分层模型、以太网帧结构、MAC 地址、可插拔 `netdev_ops_t`（init/send/recv/get_mac/link_status）+ dispatch 层 | |
| H-2 | E1000 驱动后端 | PCI 设备枚举、E1000 MMIO 寄存器映射、TX/RX 描述符环（DMA）、中断驱动收发、验证收发以太网帧 | |
| H-3 | virtio-net 驱动后端 *(备选)* | virtio PCI 设备发现、virtqueue 机制、`KCONFIG_NETDEV_BACKEND` 切换 | |

#### H-β：TCP/IP 协议栈 (ch31)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| H-4 | 协议栈概念 + `proto_ops_t` 接口 | 协议栈分层（Link→Network→Transport）、可插拔 `proto_ops_t`（send/recv/bind/connect）+ 协议注册表、sk_buff 网络缓冲区设计 | |
| H-5 | ARP + IP + ICMP | ARP 请求/应答 + 缓存表、IPv4 头解析/构建 + 路由表（单网关）、ICMP echo（ping 功能）、验证 `ping 10.0.2.2`（QEMU user-net 网关） | |
| H-6 | UDP + TCP | UDP 简单收发（无连接）、TCP 状态机（三次握手/四次挥手）、滑动窗口 + 超时重传（简化版）、验证 TCP 连接到外部服务 | |

#### H-γ：Socket API (ch32)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| H-7 | BSD Socket 概念 + `socket_ops_t` 接口 | Socket 抽象（地址族 AF_INET / 类型 SOCK_STREAM|SOCK_DGRAM）、可插拔 `socket_ops_t`（create/bind/listen/accept/connect/send/recv/close）+ dispatch 层 | |
| H-8 | INET Socket 后端 | `PF_INET` socket 实现、sockaddr_in 地址绑定、TCP listener + accept 循环、UDP sendto/recvfrom、fd 集成（socket fd 纳入 VFS）、验证 echo server | |

#### H-δ：网络工具 (ch33)

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| H-9 | 用户态 ping | ICMP echo syscall 封装、命令行参数解析、RTT 统计、`/bin/ping` 实现 | |
| H-10 | 简易 HTTP client | TCP socket + HTTP/1.0 GET 请求、响应解析、`/bin/wget` 最小实现、验证从 QEMU host 下载文件 | |

#### H-ζ：集成测试

| # | 名称 | 内容 | 状态 |
|---|------|------|------|
| H-11 | 网络子系统集成测试 | E1000 收发帧、ARP 解析、ICMP ping 往返、TCP 连接建立+数据传输、Socket fd 与 VFS 集成、`net test` shell 命令 | |

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
