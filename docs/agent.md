# SZY-KERNEL Agent 协作规约

> 项目愿景、路线图和已完成 Stage 总结见 [docs/roadmap.md](roadmap.md)。
> 本文档仅定义 **AI Agent 在协助开发时必须遵守的规则**。

---

## A. 学习级注释规范（强制）

Agent 在生成涉及底层硬件（分页、TSS、中断门）的代码时，必须包含以下注释：

- **[WHY]**：为什么要操作这个寄存器/标志位？
- **[CPU STATE]**：此操作后 CPU 的特权级、地址空间或堆栈发生了什么变化？
- **[BITFIELDS]**：对页表项 (PTE) 或描述符位定义的逐位解释

## B. 内存管理准则

- **禁止 Hardcode**：位图位置必须根据 mmap 动态计算，不能硬编码物理地址
- **对齐要求**：所有 Page 级别操作必须满足 4096 字节对齐

## C. clangd / compile_commands 同步规则（强制）

本项目使用 **clangd** 做代码导航与诊断，"真相来源"是 `compile_commands.json`。

- **每次新增/删除/移动 C/ASM 源文件**后，必须更新 `compile_commands.json`
- **每次调整编译参数**后，必须更新 `compile_commands.json`
- 更新后若 VS Code 仍异常，执行 **clangd: Restart language server**

生成方式：
- `make compdb`（依赖 `bear`）
- `AUTO_COMPDB=1 make iso`（自动刷新）

## D. 变更记录同步（强烈推荐）

- 改动对外接口/行为时，顺手在 `docs/changelog.md` 追加一条记录
- 目录结构变化时，同步更新本文档"项目文件结构"

## E. Git 工作流（强制）

所有改动 **禁止直接提交到 `main` 分支**。流程：

1. **新建功能分支**：从 `main` 创建，命名 `<stage>/<简短描述>`
2. **在功能分支上开发、提交**
3. **推送并创建 PR**：`git push -u origin <分支名>` + `gh pr create`
4. **等待用户 Review**：创建 PR 后，Agent **必须停下来**，把 PR 链接给用户，等用户确认
5. **用户确认后才能合并**：只有用户明确说"合并"/"merge"，Agent 才可以执行 `gh pr merge`。**禁止 Agent 自行合并 PR**

---

## F. 项目文件结构（同步更新）

```text
my_microkernel/
├── Makefile               # 薄 wrapper，转发到 src/Makefile
├── README.md
├── docs/                  # 项目文档
├── scripts/               # 构建/调试/测试脚本
└── src/
    ├── Makefile           # 真正的构建系统
    ├── boot/              # Multiboot2 引导 & 链接脚本
    ├── include/           # 公共头文件
    │   ├── arch/          #   x86 体系结构 (gdt/idt/irq/pic/io)
    │   ├── drivers/       #   设备驱动 (keyboard/serial)
    │   └── kernel/        #   内核通用 (pmm/vmm/shell/printk/cmd/…)
    └── kernel/            # 内核实现
        ├── arch/          #   CPU 初始化 & 汇编 stubs
        ├── cmds/          #   Shell 命令模块
        ├── core/          #   内核核心 (kmain/pmm/vmm/shell/…)
        └── drivers/       #   设备驱动实现
```
