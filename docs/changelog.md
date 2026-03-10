
## 2026-03-10
- 新增 `linker.ld`：i386/Multiboot2 内核链接脚本（1MiB 链接地址，保留 `.multiboot_header`）。
- 新增 `.gitignore`：忽略常见构建产物与编辑器文件。
- 新增 `docs/howto.md`：构建 ISO、QEMU 启动与常见问题排查。
- 更新 `README.md`：增加 Quick Start 与 how-to 入口。
- 阶段 1（1~4）：加入最小 C 内核入口 `kmain`、COM1 串口输出，并从 `_start` 传入 Multiboot2 magic/info 指针并打印 tag 列表。
- 修复 `src/boot/boot.asm`：补全 Multiboot2 header（长度/校验和/结束 tag），便于 GRUB 识别。
- 调整 `linker.ld`：将 `.multiboot_header` 放入可加载的 `.text` 段最前，并增加常见段与对齐。
- 更新 `Makefile`：使用 `grub-mkrescue` 生成 ISO，并用 QEMU `-cdrom` 启动。
