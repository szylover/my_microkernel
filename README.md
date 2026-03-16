# SZY-KERNEL

一个从零实现的 **x86 微内核操作系统**，目标是最终能自举编译 GCC、运行 Vim、支持 TCP/IP 网络。

当前处于 **里程碑 C（高级内存管理）** 阶段——内核已运行在高半区 `0xC0000000+`，拥有完整的物理 / 虚拟内存管理和交互式 Shell。

## 快速开始

```bash
# 安装依赖 (Debian/Ubuntu/WSL)
sudo apt install -y nasm binutils grub-pc-bin xorriso qemu-system-x86 gcc-multilib

# 构建 & 运行
make iso && make run
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
