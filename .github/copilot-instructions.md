# Git / PR 工作流

当用户说"提交"、"commit"、"推送"、"push"、"PR"、"合并"、"merge" 等关键词时，按以下流程操作：

## 提交 + 推送 + 创建 PR

1. **创建分支**：从当前 `main` 分支创建新的 feature 分支，命名格式 `stage-N/简短描述`（如 `stage-9/vma-interface`）
2. **暂存 + 提交**：`git add -A && git commit -m "描述"`，commit message 用英文，简洁明了
3. **推送**：`git push -u origin <branch>`
4. **创建 PR**：使用 `gh pr create --base main --head <branch> --title "标题" --body "描述"`
5. **输出 PR 链接**给用户

## 仅合并（用户说"merge"）

在创建 PR 后（或用户指定已有 PR 时），执行：
```
gh pr merge <pr-number-or-url> --squash --delete-branch
```

## 注意事项

- 创建 PR 前先确认 `make iso` 编译通过（在 `src/` 目录下）
- commit message 格式：`stage N: 简短英文描述`（如 `stage 9: add VMA pluggable interface + dispatch + cmd`）
- PR title 和 commit message 保持一致
- PR body 用中文列出本次改动要点（新增文件、修改文件、关键变更）
- 如果用户同时说了 push 和 merge，一步到位全做完

---

# Merge 前置检查清单（强制）

每次创建 PR 或执行 merge 之前，必须逐项确认：

- [ ] `docs/changelog.md` — 新增/删除文件、改接口、改行为时，必须追加当日记录
- [ ] `docs/roadmap.md` — Stage 状态变化时，更新对应行的状态标记（如 ✅）
- [ ] `compile_commands.json` — 新增/删除 `.c`/`.asm` 文件时，运行 `make compdb` 重新生成
- [ ] `book/main.pdf` — 修改了 `book/` 下任何 `.tex` 文件时，在 `book/` 目录运行 `make` 重新生成 PDF
- [ ] 下方"项目文件结构"中的目录树 — 目录或文件有变化时同步更新

---

# 代码规范

## 学习级注释规范（强制）

生成涉及底层硬件（分页、TSS、中断门）的代码时，必须包含以下注释：

- **[WHY]**：为什么要操作这个寄存器/标志位？
- **[CPU STATE]**：此操作后 CPU 的特权级、地址空间或堆栈发生了什么变化？
- **[BITFIELDS]**：对页表项 (PTE) 或描述符位定义的逐位解释

## 内存管理准则

- **禁止 Hardcode**：位图位置必须根据 mmap 动态计算，不能硬编码物理地址
- **对齐要求**：所有 Page 级别操作必须满足 4096 字节对齐

## clangd / compile_commands 同步规则（强制）

- **每次新增/删除/移动 C/ASM 源文件**后，必须更新 `compile_commands.json`
- 更新方式：`make compdb`（依赖 `bear`）或 `AUTO_COMPDB=1 make iso`

## Book / LaTeX 图片管理规范（强制）

所有 TikZ 图片代码**必须**放在 `book/figures/chNN/` 目录下，按章节子目录组织：

```
figures/
├── ch02/   # fig01-xxx.tex, fig02-xxx.tex, …
├── ch03/
├── …
└── ch11/
```

- **命名规则**：`figNN-<label-slug>.tex`（如 `fig01-vaddr-split.tex`）
- **文件内容**：仅包含 `\begin{tikzpicture}...\end{tikzpicture}` 绘图代码，不含 `\begin{figure}` / `\caption` / `\label`
- **章节引用方式**：在 section `.tex` 文件中用 `\begin{figure}[H]\centering\input{figures/chNN/figNN-slug}\caption{...}\label{...}\end{figure}`
- **新增/修改图片时**：直接编辑 `figures/chNN/` 下的文件，不要在 section 文件中内联 TikZ 代码
- **遍历所有图片**：只需扫描 `figures/` 目录即可，无需遍历所有章节源码

## 变更记录同步（强烈推荐）

- 改动对外接口/行为时，在 `docs/changelog.md` 追加当日记录
- 目录结构变化时，同步更新下方"项目文件结构"

---

# 项目文件结构（同步更新）

```text
my_microkernel/
├── .github/
│   └── copilot-instructions.md   # 本文件 — AI Agent 协作规约
├── Makefile                       # 薄 wrapper，转发到 src/Makefile
├── README.md
├── book/                          # LaTeX 书稿 《自己动手写操作系统》
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
    │   ├── arch/                  #   x86 体系结构 (gdt/idt/irq/pic/io)
    │   ├── drivers/               #   设备驱动 (keyboard/serial)
    │   └── kernel/                #   内核通用 (pmm/vmm/vma/kmalloc/shell/printk/cmd/kconfig/…)
    └── kernel/                    # 内核实现
        ├── arch/                  #   CPU 初始化 & 汇编 stubs
        ├── cmds/                  #   Shell 命令模块
        ├── core/                  #   内核入口和基础设施 (kmain/shell/printk/console)
        ├── drivers/               #   设备驱动实现
        └── mm/                    #   内存管理 (pmm/pmm_bitmap/pmm_buddy/vmm/vma/vma_sorted_array/vma_rbtree/vma_maple/kmalloc/heap_first_fit/heap_slab/mmap)
```
