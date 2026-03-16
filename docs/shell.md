# Shell 设计文档

> 交互式内核 Shell（类似 CMD / Linux shell），运行在 Ring 0。

## 架构

```
输入层 (console.c)     keyboard IRQ1 + serial 轮询 → console_getc()
        │
行编辑层 (shell.c)     回显 / Backspace / Enter → 行缓冲 line[128]
        │
命令解析 (shell.c)     空格分隔 → argv[0]=cmd, argv[1..]=args
        │
命令分发 (shell.c)     遍历 g_cmds[] 注册表，匹配 name 后调用 fn()
        │
命令实现 (cmds/*.c)    每个命令一个独立编译单元，导出 const cmd_t
```

## 命令接口

定义在 `src/include/kernel/cmd.h`：

```c
typedef int (*cmd_fn_t)(int argc, char** argv);
typedef struct { const char* name; const char* help; cmd_fn_t fn; } cmd_t;
```

Shell 维护 `const cmd_t*` 数组，按 `name` 分发。

## 已实现命令

| 命令 | 文件 | 用途 |
|------|------|------|
| `cls` | cmd_cls.c | 清屏（ANSI 转义序列） |
| `shutdown` | cmd_shutdown.c | QEMU/Bochs 关机或 halt |
| `cmds` | cmd_cmds.c | 列出所有命令 |
| `mmap` | cmd_mmap.c | 显示 Multiboot2 内存映射 |
| `free` | cmd_free.c | 物理内存用量 |
| `pmm` | cmd_pmm.c | PMM 调试（state/alloc/free/dump） |
| `vmm` | cmd_vmm.c | VMM 调试（state/lookup/pd/pt/map/unmap/fault） |
| `heap` | cmd_heap.c | 内核堆调试（status/alloc/free/test） |
| `vma` | cmd_vma.c | VMA 调试（list/find/count/test） |

## 添加新命令

1. 创建 `src/kernel/cmds/cmd_xxx.c`
2. 实现函数并导出 `const cmd_t cmd_xxx = { .name = "xxx", .help = "...", .fn = cmd_xxx_fn };`
3. 在 `shell.c` 中 `extern const cmd_t cmd_xxx;` 并加入 `g_cmds[]` 数组
4. Makefile 会自动发现 `kernel/cmds/*.c`，无需手动添加

详细命令用法见 [memory-commands.md](memory-commands.md)。
