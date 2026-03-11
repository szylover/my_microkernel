# 微内核操作系统开发规约 (v4.0 - 工业级学习版)

## 1. 核心愿景：从“裸机打印”到“加载程序”

* **终极目标**：实现一个能解析 **ELF32** 文件、切换至 **Ring 3 (用户态)** 并通过 **Syscalls (系统调用)** 与内核交互的微内核。
* **自研原则**：拒绝魔法数字。所有内存分配（位图/红黑树）、分页机制、进程切换必须由项目原生代码实现。

---

## 2. 阶段化路线图 (Agent 引导优先级)

### 第一阶段：交互与环境 (已达成)

* **状态**：完成 GDT/IDT、串口/键盘、CMD 命令解析、Multiboot2 内存探测。

### 第二阶段：物理内存“批发商” (PMM) —— **当前重点**

* **核心逻辑**：基于 `mmap` 找出的 `best_region` 建立 **Bitmap (位图)** 管理器。
* **接口要求**：`pmm_alloc_page()` (分配 4KB 页), `pmm_free_page()`。
* **验证**：CMD 增加 `free` 指令，实时查看剩余物理页数。

### 第三阶段：虚拟内存“幻境” (VMM & Paging)

* **核心逻辑**：开启 x86 分页。实现 **Identity Mapping** (保证内核不崩) 和 **High-Half Kernel** (内核映射至 `0xC0000000`)。
* **接口要求**：`vmm_map(virt, phys, flags)`。

### 第四阶段：内存“零售商” (Heap & 红黑树)

* **核心逻辑**：在内核态实现 `kmalloc`。
* **关键点**：使用 **红黑树 (Red-Black Tree)** 管理不连续的空闲内存块，平衡分配效率与内存碎片。

### 第五阶段：执行外部代码 (ELF & Ring 3)

* **核心逻辑**：
1. **ELF 解析**：读取 `.text` 和 `.data` 段。
2. **TSS 设置**：为特权级切换准备环境。
3. **用户态跳转**：利用 `iret` 压栈从内核态降级到用户态运行。



---

## 3. 技术约束与协作指令

### 3.1 学习级注释规范 (强制执行)

Agent 在生成涉及底层硬件（分页、TSS、中断门）的代码时，必须包含以下注释：

* **[WHY]**：为什么要操作这个寄存器/标志位？（如：开启 CR0.PG 的后果）。
* **[CPU STATE]**：此操作后，CPU 的特权级、地址空间或堆栈发生了什么变化？
* **[BITFIELDS]**：对页表项 (PTE) 或描述符位定义的逐位解释（如：Present, Writable, User 位）。

### 3.2 内存管理准则

* **禁止 Hardcode**：位图的位置必须根据 `mmap` 动态计算，不能硬编码物理地址。
* **对齐要求**：所有 Page 级别的操作必须满足 **4096** 字节（4KB）对齐。

### 3.3 clangd / compile_commands 同步规则（强制）

本项目默认使用 **clangd** 做 C 代码导航与诊断；其“真相来源”是仓库根目录的 `compile_commands.json`。

**你要的“每次改动也更新下”具体指：**

* **每次新增/删除/移动 C/ASM 源文件**（`src/**/*.c` / `src/**/*.asm`）后，必须更新 `compile_commands.json`。
* **每次调整编译参数**（如 `Makefile` 里的 `CFLAGS`/`-I...`/`CC`）后，必须更新 `compile_commands.json`。
* 更新后若 VS Code 仍然跳转/诊断异常，执行一次 **clangd: Restart language server**。

生成方式（二选一）：

* 手动生成：`make compdb`（依赖 `bear`；Debian/Ubuntu: `sudo apt install -y bear`）
* 自动生成：`AUTO_COMPDB=1 make iso`（在每次 `make iso` 前自动刷新 `compile_commands.json`）

补充说明：

* `compile_commands.json` 在 `.gitignore` 里，一般不提交；换机/拉新分支需要重新生成。
* `.clangd` 提供的是 **fallback 编译参数**，用于 clangd 在数据库缺失时尽量不“红一片”，但不应替代 `compile_commands.json`。

### 3.4 变更记录同步（建议，但强烈推荐）

* **每次改动对外接口/行为**（新增命令、修改输出格式、改初始化顺序等），顺手在 `docs/changelog.md` 追加一条 bullet。
* **每次目录结构发生变化**（新增模块/文件夹）时，同步更新本文档第 5 节“项目文件结构”。
### 3.5 Git 工作流（强制）

所有改动 **禁止直接提交到 `main` 分支**。必须遵循以下流程：

1. **新建功能分支**：从 `main` 创建分支，命名格式 `<stage>/<简短描述>`（如 `stage3/vmm-identity-paging`、`stage4/kmalloc-rbtree`）。
2. **在功能分支上开发、提交**：可以有多个 commit，保持每个 commit 有意义的粒度。
3. **推送并创建 PR**：`git push -u origin <分支名>`，然后用 `gh pr create --base main --head <分支名>` 创建 Pull Request，PR 标题和描述要清晰说明变更内容。
4. **Review 后合并**：用户确认后再 merge 到 main（可通过 GitHub 页面或 `gh pr merge`）。

Agent 在执行任务时如果发现当前在 `main` 分支上有未提交的改动，应先创建功能分支再继续。
---

## 4. 协作 Prompt 模板 (可以直接复制给 Agent)

* **任务下达**：“请基于我 `src/kernel/core/mmap.c` 中挑选可用内存区间（best region）的逻辑，生成 `pmm.c` 并实现位图初始化，提供 `pmm_alloc_page/pmm_free_page` 接口。请遵守 `agent.md` 的学习级注释规范。”
* **调试求助**：“我在开启分页后，QEMU 发生了 Triple Fault（自动重启）。请分析我的分页初始化流程（将来放在 `vmm.c`），重点检查内核代码段的 Identity Mapping 是否正确。”
* **算法移植**：“帮我实现一个红黑树的插入和删除算法，用于管理内存块。注意：内核当前没有 `malloc`，请使用我预留的静态缓冲区或 PMM 提供的页空间。”

---

## 5. 项目文件结构 (同步更新)

```text
my_microkernel/
├── linker.ld          # <--- 核心：定义内核物理/虚拟加载布局
├── Makefile           # 支持 make compdb 生成 clangd 索引
├── include/
│   ├── arch/          # x86 体系结构相关（端口 IO / 描述符 / 中断 / PIC）
│   │   ├── gdt.h      # GDT 描述符与初始化接口
│   │   ├── idt.h      # IDT 描述符与安装接口
│   │   ├── irq.h      # IRQ 分发与 handler 注册
│   │   ├── pic.h      # 8259 PIC remap/mask/EOI
│   │   └── io.h       # inb/outb/io_wait 等端口 IO
│   ├── drivers/       # 设备驱动（键盘/串口）
│   │   ├── keyboard.h # PS/2 键盘（IRQ1）ASCII 缓冲与接口
│   │   └── serial.h   # COM1 串口初始化与收发
│   └── kernel/        # 内核通用层（不直接碰硬件寄存器）
│       ├── cmd.h      # shell 命令接口（cmd_t/cmd_fn_t）
│       ├── console.h  # 输入聚合（键盘缓冲 + 串口轮询）
│       ├── mmap.h     # Multiboot2 mmap dump/summary（给 `mmap` 命令用）
│       ├── printk.h   # 内核打印（当前后端：serial）
│       └── shell.h    # 交互式 shell 入口
└── src/
    ├── boot/
    │   └── boot.asm           # Multiboot2 header + 设置栈 + 调用 kmain
    └── kernel/
        ├── arch/              # 体系结构层（与 CPU/中断控制器强相关）
        │   ├── gdt.c          # GDT 构建与加载（lgdt + 段寄存器刷新）
        │   ├── gdt_flush.asm  # far jump/段寄存器刷新辅助
        │   ├── idt.c          # IDT 构建与加载（lidt + ISR 安装）
        │   ├── isr_stubs.asm  # 0..31 异常 ISR 汇编桩（保存现场 -> C handler）
        │   ├── irq.c          # IRQ 分发、handler 注册、统一入口
        │   ├── irq_stubs.asm  # IRQ0..15 汇编桩（进入 irq.c）
        │   └── pic.c          # 8259 PIC remap/mask/EOI
        ├── drivers/           # 设备驱动层
        │   ├── serial.c       # COM1 驱动（printk 后端）
        │   └── keyboard.c     # PS/2 键盘 IRQ1 驱动（scancode->ASCII + ring buffer）
        ├── core/              # 内核通用逻辑（可移植/与硬件弱耦合）
        │   ├── kmain.c        # C 入口：初始化顺序、保存 mb2 指针、启动 shell
        │   ├── printk.c       # printk 格式化与输出
        │   ├── console.c      # console_getc/try_getc：键盘 + 串口输入聚合
        │   ├── shell.c        # shell：行编辑、tokenize、命令分发、注册表
        │   └── mmap.c         # Multiboot2 mmap 解析与 best region 摘要打印
        └── cmds/              # shell 内置命令（每个命令一个编译单元）
            ├── cmd_cls.c      # cls：清屏（当前为 ANSI 序列）
            ├── cmd_cmds.c     # cmds：列出可用命令
            ├── cmd_mmap.c     # mmap：调用 mmap_print() 输出内存地图
            └── cmd_shutdown.c # shutdown：尝试触发 QEMU/Bochs 关机

```

---

### 💡 建议的下一步：

既然目录结构已经按功能分层，你可以直接把你的 `src/kernel/core/mmap.c` 发给我，然后发送第一条指令：
**“按照 agent.md 第二阶段要求，基于 best region 实现 PMM 位图分配器，并给 shell 增加 free 指令。”**