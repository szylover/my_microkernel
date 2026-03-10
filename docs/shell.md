# Shell 设计文档（cmd 风格）

本文档描述你要做的“内核交互式 shell”（类似 Windows CMD / Linux shell）的目标 UX、模块分层、以及分阶段落地计划。


## 0. 你想要的最终效果（UX）

- 内核打印完启动信息后，进入交互循环。
- 每一行以固定提示符开头：

```
szy-kernel > 
```

- 系统等待你输入一行命令（支持退格/回显）。
- 你输入命令并回车后，执行对应指令。
- 指令必须可扩展：
  - 第一批：`cls`（先落地最小闭环）
  - 下一批：`help`、`info`
  - 第二批：`mmap`（输出类似 `Available RAM: 0x100000 - 0x7EE0000`）
  - 第二批：`cpu`（输出 CPU 信息）


## 1. 现状（截至目前仓库代码）

- 输出：通过串口 `COM1(0x3F8)`，`printk()` -> `serial_putc()`。
- 中断：已安装 CPU 异常 0..31 的 IDT stubs。
- 新增（本次改动）：接入 PIC/IRQ 框架、IRQ1 键盘 scancode->ASCII（带环形缓冲），并实现最小 shell（目前仅 `cls`）。


## 2. 分层设计（易扩展 + 易验证）

### 2.1 输入层（Input Provider）

最终你会有两种输入源：

1) **PS/2 键盘（IRQ1）**
- 通过 IRQ1 进入 keyboard handler，从端口 `0x60` 读取 scancode
- scancode -> key state（shift/ctrl/alt）-> ASCII 字符流

2) **串口（轮询）**（可作为早期/救援通道）
- 在没有键盘驱动时，用串口轮询也能输入命令（QEMU `-serial stdio`）
- 好处：调试非常稳，适合早期把 shell 跑起来

**建议接口（后续实现）：**
- `int console_try_getc(char* out)` / `char console_getc()`
- 初期实现可直接让 shell 依赖 `keyboard_getc()` 或 `serial_getc()`，后续再抽象。


### 2.2 行编辑层（Line Editor）

最小能力：
- 回显可见字符
- Backspace 删除
- Enter 提交

建议：
- 固定行缓冲 `line[128]` 或 `line[256]`
- 溢出策略：忽略后续字符（先简单）


### 2.3 命令解析层（Parser）

最小 parser：
- 空格分隔 token：`argv[0]=cmd`、`argv[1..]=args`
- 先不支持引号/转义（后续再加）


### 2.4 命令分发层（Dispatcher / Registry）

核心目标：命令可扩展，不要写一堆 if/else。

建议结构：

- `struct cmd { const char* name; const char* help; int (*fn)(int argc, char** argv); }`
- `cmd_table[]` 静态数组
- `help` 遍历 `cmd_table[]` 打印


## 3. 内置命令设计

### 3.1 `help`
- 输出命令列表与简短说明

### 3.2 `info`
- 输出内核版本信息、构建时间、当前输入方式（keyboard/serial）等

### 3.3 `cls`
两种实现路线（二选一或都支持）：
- 串口终端：发 ANSI 清屏序列 `\x1b[2J\x1b[H`
- VGA 文本模式：清 `0xb8000` 文本缓冲


## 4. 扩展命令规划

### 4.1 `mmap`

来源：Multiboot2 Memory Map Tag（常见 type=6）。

目标输出（示例）：

```
Available RAM: 0x100000 - 0x7EE0000
```

实现要点：
- 在 `kmain` 已拿到 `mb2_info` 指针
- 需要解析 tag 内容，找到可用区间（type=1）并打印

### 4.2 `cpu`

来源：`cpuid` 指令。

最小输出建议：
- vendor string
- family/model/stepping
- 关键 feature 位（可后续再加）


## 5. 分阶段落地计划（你现在正在做的顺序）

### 阶段 A：先打通 IRQ1 键盘中断（已完成）

目的：验证硬件中断链路：
- IDT 安装 0x20..0x2F 的 IRQ stubs
- PIC remap + mask + EOI
- IRQ1 handler 能稳定触发

验证点：按键能触发 IRQ1，handler 能从 0x60 读走 scancode，并（在中断里）翻译为 ASCII 写入缓冲区。


### 阶段 B：实现 shell 最小闭环（help/info/cls）（已完成）

- 输入层：keyboard ASCII（IRQ1 -> ring buffer -> `keyboard_getc()`）
- 行编辑：回显/退格/回车
- 命令表：help/info/cls


### 阶段 C：扩展 mmap/cpu

- `mmap`：解析 Multiboot2 mmap tag
- `cpu`：实现 cpuid 输出


## 6. 验证方法（建议）

- 启动：`make iso && make run`
- IRQ1 测试：在 QEMU 窗口里按键，串口应输出 `SzyOs > `
- 后续 shell：输入 `help` / `info` / `cls` 验证
