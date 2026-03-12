# How-to: Build & Run (GRUB + QEMU)

本项目当前是一个 **i386 Multiboot2 内核**（NASM 汇编 + ld 链接），通过 **GRUB** 打包成 ISO，并用 **QEMU** 启动。

当前已经包含最小 C 入口 `kmain`，并通过串口（COM1/0x3F8）输出调试信息。

## 1. 环境依赖

在 Debian/Ubuntu 上：

```bash
sudo apt update
sudo apt install -y nasm binutils grub-pc-bin xorriso qemu-system-x86 gcc-multilib
```

说明：
- `grub-mkrescue` 来自 `grub-pc-bin`
- `grub-mkrescue` 生成 ISO 通常还需要 `xorriso`

### 可选：开发工具

```bash
# clangd 代码索引（需要 bear 生成 compile_commands.json）
sudo apt install -y bear

# GitHub CLI（用于从命令行创建 PR、合并分支等）
# 安装步骤（官方源）：
sudo mkdir -p -m 755 /etc/apt/keyrings
wget -nv -O- https://cli.github.com/packages/githubcli-archive-keyring.gpg \
  | sudo tee /etc/apt/keyrings/githubcli-archive-keyring.gpg > /dev/null
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
  | sudo tee /etc/apt/sources.list.d/github-cli.list > /dev/null
sudo apt update && sudo apt install -y gh
```

安装后首次使用需要登录：

```bash
gh auth login
```

按提示选择 `GitHub.com` → `HTTPS` → `Login with a web browser`，浏览器会打开授权页面。

常用命令：

```bash
gh pr create --base main --head <分支名>   # 创建 PR
gh pr list                                  # 查看 PR 列表
gh pr merge <PR编号>                        # 合并 PR
```

## 2. 构建 ISO

在项目根目录执行：

```bash
make clean
make iso
```

产物：
- `build/kernel.iso`
- `build/isodir/boot/kernel.elf`

## 3. 启动 QEMU

```bash
make run
```

预期现象：
- QEMU 窗口左上角能看到 `OK`（写入 VGA 文本显存 0xb8000）
- 终端（`-serial stdio`）能看到类似 `kmain: hello from C` 和 Multiboot2 tag 列表
- 退出：关闭窗口或在终端 `Ctrl+C`

## 4. 常见问题排查

### 4.1 提示缺少 grub-mkrescue / xorriso

- `Missing tool: grub-mkrescue`：安装 `grub-pc-bin`
- `xorriso` 相关报错：安装 `xorriso`

### 4.2 汇编报错像是 "no such instruction: section"

这通常是用 GNU `as` 去汇编 NASM 语法导致的。
- 本项目 Makefile 已显式使用 `NASM ?= nasm`
- 请确认本机安装了 `nasm`，且 `make tools` 不报错

### 4.3 GRUB 进不去 / 黑屏

优先检查：
- `src/boot/boot.asm` 是否包含完整 Multiboot2 header（包含 end tag）
- `src/boot/linker.ld` 是否保证 `.multiboot_header` 在可加载段的前部并 `KEEP()`

## 5. 目录与文件

- `src/boot/boot.asm`：Multiboot2 入口与最小裸机验证输出
- `src/boot/linker.ld`：i386 链接脚本（入口 `_start`，高半区链接地址）
- `Makefile`：构建 ELF、打包 GRUB ISO、启动 QEMU
