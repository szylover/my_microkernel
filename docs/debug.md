# 内核调试指南（QEMU + GDB）

这份文档描述如何在本项目中使用 QEMU 的 gdbstub 配合 GDB 做断点、单步和现场检查。

## 0. 前置条件

- 需要安装：`qemu-system-i386`、`gdb`、`grub-mkrescue`。
- 建议使用支持 32-bit 的 GDB：
  - 常见发行版的 `gdb` 即可。
  - 如果你的系统默认 GDB 对架构支持不完整，可用 `gdb-multiarch`（不同发行版包名不同）。

## 1. 最常用：两终端调试工作流

### 终端 A：启动 QEMU 并等待 GDB

```bash
make DEBUG=1 debug
```

- `DEBUG=1` 会启用 `-O0 -g3` 等选项，单步/回溯更稳定。
- `debug` 会以 `-S` 启动 QEMU：CPU 暂停在复位态，等待 GDB attach。

### 终端 B：启动 GDB 并自动连接

```bash
make gdb
```

- 会加载 `build/isodir/boot/kernel.elf` 的符号并连接到 QEMU 的 gdbstub。
- 默认端口是 `1234`（由 `Makefile` 的 `QEMU_GDB_PORT` 控制；GDB 脚本里也写死为 `:1234`）。

## 2. 断点与单步（C 级 + 指令级）

### 基本控制

- 继续运行：`c`
- 暂停运行：在 GDB 里按 `Ctrl+C`

### 断点

- 在函数入口断下：`b kmain`
- 在 PMM 初始化断下：`b pmm_init`
- 查看断点：`info break`
- 禁用/启用断点：`disable 1` / `enable 1`
- 删除断点：`del 1`

### 单步

- C 级单步（更“语义化”）：
  - `n`：next（跨过函数调用）
  - `s`：step（进入函数）
  - `finish`：运行到当前函数返回

- 指令级单步（早期内核/中断/异常更可靠）：
  - `si`：stepi（单步一条指令）
  - `ni`：nexti（下一条指令，尽量不进入 call）
  - 查看接下来指令：`x/10i $eip`

## 3. 现场检查：寄存器、栈、内存、回溯

### 寄存器

- `info reg`
- 查看当前指令指针附近：`x/16i $eip`

### 栈与回溯

- 回溯：`bt`
- 查看栈内存：`x/32wx $esp`

> 注意：不开 `DEBUG=1` 时，优化会让 `bt`、单步与局部变量显示变差。

### 变量与地址

- 以十六进制打印表达式：`p/x some_var`
- 打印指针：`p/a ptr`

## 4. 推荐断点点位（排查“卡住/无输出”特别有用）

- `kmain`：确认启动流程有没有走到内核主入口
- `pmm_init`：确认是否在 PMM 初始化阶段死机/覆盖内存
- `pic_init` / `keyboard_init`：确认中断控制器/键盘初始化
- `shell_run`：确认是否进入交互

## 5. 常见问题（FAQ）

### 5.1 GDB 连不上（Connection refused / timeout）

- 先确认你已经在终端 A 执行了 `make debug`，并且 QEMU 没退出。
- 端口不一致时：
  - `make debug QEMU_GDB_PORT=1234` 与 `tools/kernel.gdb` 里 `target remote :1234` 必须一致。

### 5.2 单步“跳来跳去”或 `bt` 很奇怪

- 用 `make DEBUG=1 debug` 重新启动（关闭优化，保留符号）。

### 5.3 断点打不住

- 确认断点符号存在：`info functions kmain`
- 如果你在汇编里想断某个 label，优先用 `disassemble` 找到地址，再 `b *0x...`。

## 6. 附：项目内置的 GDB 脚本

- 脚本位置：`tools/kernel.gdb`
- 它会做：设置架构为 i386、加载符号、连接 `:1234`、在 `kmain` 下断。
