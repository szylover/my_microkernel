
## 2026-03-11
- 阶段 3（VMM 起步）：新增 `include/kernel/vmm.h`（页目录/页表类型、PDE/PTE flags、vmm_init/vmm_map_page/vmm_unmap_page API）。
- 阶段 3（VMM 实现）：新增 `src/kernel/core/vmm.c`（identity mapping 0–16MiB + 开启分页）、`src/kernel/arch/paging_flush.asm`（CR3 加载、CR0.PG 置位、invlpg TLB 刷新）。
- 更新 `kmain.c`：在 pmm_init() 后调用 vmm_init() 开启分页。

## 2026-03-10
- 新增 `linker.ld`：i386/Multiboot2 内核链接脚本（1MiB 链接地址，保留 `.multiboot_header`）。
- 新增 `.gitignore`：忽略常见构建产物与编辑器文件。
- 新增 `docs/howto.md`：构建 ISO、QEMU 启动与常见问题排查。
- 更新 `README.md`：增加 Quick Start 与 how-to 入口。
- 阶段 1（1~4）：加入最小 C 内核入口 `kmain`、COM1 串口输出，并从 `_start` 传入 Multiboot2 magic/info 指针并打印 tag 列表。
- 阶段 2（GDT 起步）：新增 GDT 初始化（平坦 code/data 段）与 `lgdt`/far jump 刷新段寄存器，并在串口输出 before/after 便于验证。
- 更新 `docs/agent.md`：追加“学习级注释规范”。
- 阶段 2（IDT 起步）：新增 IDT 初始化与 0..31 异常 ISR stubs，串口打印异常向量/错误码/EIP，并用 `int3` 做自检。
- 修复 `src/boot/boot.asm`：补全 Multiboot2 header（长度/校验和/结束 tag），便于 GRUB 识别。
- 调整 `linker.ld`：将 `.multiboot_header` 放入可加载的 `.text` 段最前，并增加常见段与对齐。
- 更新 `Makefile`：使用 `grub-mkrescue` 生成 ISO，并用 QEMU `-cdrom` 启动。
- 阶段 2（IRQ 起步）：新增 8259 PIC remap/mask/EOI、IRQ0..15 汇编 stubs，并在 IDT 安装 0x20..0x2F。
- 阶段 2（Keyboard 起步）：新增 IRQ1 键盘 scancode->ASCII（含 Shift）与环形缓冲区，对外提供 `keyboard_getc/keyboard_try_getc`。
- 阶段 2（Shell 起步）：新增最小交互式 shell（`szy-kernel >`，行编辑 + 命令表），当前仅实现 `cls`（其余命令按 docs 规划逐步加回）。
- 新增 `docs/shell.md`：记录 cmd 风格 shell 的 UX/分层/命令规划（help/info/cls/mmap/cpu）与阶段落地路线。
- 阶段 2（Command 模块化）：新增 `include/kernel/cmd.h` 命令接口；将 `cls` 拆为独立命令模块（`src/kernel/cmds/cmd_cls.c`），shell 通过注册表调用。
- 阶段 2（Console 输入层）：新增 `include/kernel/console.h`/`src/kernel/core/console.c`，统一键盘（IRQ1 缓冲）与串口（轮询）输入；shell 只依赖 console。
- 阶段 2（Shell 命令）：新增 `shutdown` 命令模块（`src/kernel/cmds/cmd_shutdown.c`），尝试触发 QEMU/Bochs 关机，失败则 halt。
- 阶段 2（Shell 命令）：新增 `cmds` 命令模块（`src/kernel/cmds/cmd_cmds.c`），列出当前所有可用命令。
- 开发体验：新增 `.clangd`（clangd fallback 编译参数）、`make compdb`/`tools/compdb.sh`（通过 bear 生成 `compile_commands.json`），并在 `docs/agent.md` 补充 VS Code + clangd 配置说明。
- 更新 `docs/agent.md`：明确 clangd/`compile_commands.json` 的“每次改动同步更新”规则，并补充变更记录同步建议。
