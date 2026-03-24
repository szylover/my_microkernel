# Design Spec: int 0x80 系统调用后端

Stage: D-3
Date: 2026-03-24

## Overview

将 D-2 搭建的 `syscall_ops_t` 可插拔接口真正接入 IDT，实现 int 0x80 后端。
在 IDT 第 0x80 号向量安装一个 DPL=3 的中断门，允许用户态（Ring 3）通过
`int 0x80` 指令触发系统调用。汇编 stub 负责特权级切换后的寄存器保存/恢复，
C handler 提取参数构造 `syscall_regs_t` 并转发到 `syscall_dispatch()`。
完成后验证端到端路径：Ring 3 用户代码 → `int 0x80` → `sys_write("Hello from Ring 3!\n")` → `sys_exit(0)` → 内核 halt。

## Interface Definition

### 已有接口（D-2 提供，本阶段消费）

- **`syscall_regs_t`** — 系统调用参数快照（nr/arg1..arg5）
- **`syscall_ops_t`** — `{ name, init }` 后端操作表
- **`syscall_register_backend(ops)`** — 注册后端（kmain 调用）
- **`syscall_dispatch(regs)`** — 分发到 `syscall_table[nr]`，返回 `int32_t`
- **`syscall_register(nr, handler)`** — 注册单个 handler

### 本阶段新增/修改

#### 公开 `idt_set_gate()`（idt.h / idt.c）

当前 `idt_set_gate()` 是 `idt.c` 中的 `static` 函数，int0x80 后端无法调用。
需要将其改为非 static 并在 `idt.h` 中声明，供后端在 init 时安装 IDT gate。

```c
// idt.h 新增
void idt_set_gate(uint8_t vector, void (*handler)(void),
                  uint16_t selector, uint8_t type_attr);
```

#### 公开 `regs_t`（idt.h）

当前 `regs_t` 定义在 `idt.c` 内部。int0x80 的 C handler 需要解析同样布局的
栈帧，应将 `regs_t` 移到 `idt.h` 中公开，保证汇编 stub 和 C handler 使用
同一套布局定义。

```c
// idt.h 新增
typedef struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
    uint32_t user_esp, user_ss;
} regs_t;
```

#### `syscall_int0x80_get_ops()`

```c
// syscall_backends.h
const syscall_ops_t* syscall_int0x80_get_ops(void);
```

#### IDT Gate 属性常量

```c
// 0x8E = P=1, DPL=0, Type=1110 (32-bit interrupt gate) — 已有，仅内核可触发
// 0xEE = P=1, DPL=3, Type=1110 (32-bit interrupt gate) — 用户态可触发 int 0x80
#define IDT_INT_GATE_DPL0  0x8E
#define IDT_INT_GATE_DPL3  0xEE
```

#### 用户态 syscall 封装宏（`syscall_user.h`）

```c
// 放在 src/include/kernel/syscall_user.h
// 用户态代码通过 inline asm 封装 int 0x80

#define _syscall0(nr)                                           \
({                                                              \
    int32_t __ret;                                              \
    __asm__ volatile (                                          \
        "int $0x80"                                             \
        : "=a"(__ret)                                           \
        : "a"((uint32_t)(nr))                                   \
        : "memory"                                              \
    );                                                          \
    __ret;                                                      \
})

#define _syscall1(nr, a1)                                       \
({                                                              \
    int32_t __ret;                                              \
    __asm__ volatile (                                          \
        "int $0x80"                                             \
        : "=a"(__ret)                                           \
        : "a"((uint32_t)(nr)), "b"((uint32_t)(a1))              \
        : "memory"                                              \
    );                                                          \
    __ret;                                                      \
})

#define _syscall2(nr, a1, a2)                                   \
({                                                              \
    int32_t __ret;                                              \
    __asm__ volatile (                                          \
        "int $0x80"                                             \
        : "=a"(__ret)                                           \
        : "a"((uint32_t)(nr)), "b"((uint32_t)(a1)),             \
          "c"((uint32_t)(a2))                                   \
        : "memory"                                              \
    );                                                          \
    __ret;                                                      \
})

#define _syscall3(nr, a1, a2, a3)                               \
({                                                              \
    int32_t __ret;                                              \
    __asm__ volatile (                                          \
        "int $0x80"                                             \
        : "=a"(__ret)                                           \
        : "a"((uint32_t)(nr)), "b"((uint32_t)(a1)),             \
          "c"((uint32_t)(a2)), "d"((uint32_t)(a3))              \
        : "memory"                                              \
    );                                                          \
    __ret;                                                      \
})
```

> **注意**：这些宏使用 GCC statement expression `({...})`，仅适用于 GCC/Clang。
> `"memory"` clobber 确保编译器不会在 syscall 前后重排内存访问。


## Kernel Implementation Plan (@Kernel)

### New Files

| File | Purpose | Key Functions |
|------|---------|---------------|
| `src/kernel/arch/syscall_stub.asm` | int 0x80 汇编入口点 | `syscall_entry_int0x80`（全局符号，安装到 IDT[0x80]） |
| `src/kernel/arch/syscall_int0x80.c` | int 0x80 C 后端（init + handler） | `int0x80_init()`、`syscall_handler_int0x80()`、`syscall_int0x80_get_ops()` |
| `src/include/kernel/syscall_backends.h` | 后端 get_ops 声明汇总 | `syscall_int0x80_get_ops()` |
| `src/include/kernel/syscall_user.h` | 用户态 inline asm syscall 封装 | `_syscall0` ~ `_syscall3` 宏 |

### Modified Files

| File | Change | Why |
|------|--------|-----|
| `src/include/arch/idt.h` | 1) 将 `regs_t` 定义从 idt.c 移到 idt.h<br>2) 声明 `idt_set_gate()`（去掉 static） | int0x80 后端需要安装 IDT gate；C handler 需要 `regs_t` 类型 |
| `src/kernel/arch/idt.c` | 1) 删除本地 `regs_t` typedef（改用 idt.h 中的）<br>2) `idt_set_gate()` 去掉 `static`<br>3) 不再需要本地 `INT_GATE` 常量，可使用 idt.h 中定义 | 与 idt.h 公开接口保持一致 |
| `src/kernel/core/kmain.c` | 在 `syscall_init()` 前添加后端注册：<br>`#if KCONFIG_SYSCALL_BACKEND == 0`<br>`syscall_register_backend(syscall_int0x80_get_ops());`<br>`#endif`<br>并添加 `#include "syscall_backends.h"` | 激活 int0x80 后端 |
| `src/kernel/arch/user_test_code.asm` | 新增 `user_syscall_start` / `user_syscall_end` 代码片段：<br>- 在用户地址空间内持有字符串 "Hello from Ring 3!\n"<br>- 用 int 0x80 发起 `SYS_WRITE(1, msg, len)` + `SYS_EXIT(0)` | 为 `ring3 syscall` 测试命令提供用户态测试代码 |
| `src/kernel/cmds/cmd_ring3.c` | 1) 添加 `ring3 syscall` 子命令<br>2) 新函数 `ring3_syscall()` — 复制 `user_syscall_start..end` 到用户代码页，跳转到 Ring 3 执行<br>3) 更新帮助文本 | 端到端验证 int 0x80 系统调用 |

### 汇编 stub 详细设计：`syscall_stub.asm`

#### 设计理念

与 `isr_stubs.asm` / `irq_stubs.asm` 采用相同的栈帧布局（兼容 `regs_t`），
这样 C handler 可以直接用 `regs_t*` 指针访问用户寄存器，也便于将来统一调试输出。

#### 栈帧布局

当 Ring 3 执行 `int 0x80` 时，CPU 自动完成特权级切换并压栈：

```
                    高地址
                ┌─────────────┐
                │  user SS    │  ← CPU 压入（跨特权级）
                ├─────────────┤
                │  user ESP   │
                ├─────────────┤
                │  EFLAGS     │
                ├─────────────┤
                │  user CS    │
                ├─────────────┤
                │  user EIP   │  ← CPU 压入（返回地址）
                ├─────────────┤
                │  err_code=0 │  ← stub 手动 push 0
                ├─────────────┤
                │  int_no=0x80│  ← stub 手动 push 0x80
                ├─────────────┤
                │  pushad     │  ← 8 个通用寄存器
                │  (EAX..EDI) │
                ├─────────────┤
                │  DS ES FS GS│  ← 段寄存器
                └─────────────┘
                    低地址 (ESP)
```

这与 `isr_common_stub` 完全一致，因此 C handler 接收的 `regs_t*` 字段偏移正确。

#### 伪代码

```nasm
; syscall_stub.asm
bits 32
extern syscall_handler_int0x80
global syscall_entry_int0x80

syscall_entry_int0x80:
    ; 1. 补齐 regs_t 布局：push 伪 err_code 和 int_no
    push dword 0            ; err_code = 0（软中断无 error code）
    push dword 0x80         ; int_no   = 0x80

    ; 2. 保存通用寄存器（pushad 顺序：EAX ECX EDX EBX ESP EBP ESI EDI）
    pushad

    ; 3. 保存段寄存器
    push ds
    push es
    push fs
    push gs

    ; 4. 切换到内核数据段
    mov ax, 0x10            ; GDT_KERNEL_DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 5. 调用 C handler，传入 regs_t*
    push esp
    call syscall_handler_int0x80
    add esp, 4

    ; 6. 恢复段寄存器
    pop gs
    pop fs
    pop es
    pop ds

    ; 7. 恢复通用寄存器（popad 从栈上恢复，包括被 C handler 修改的 EAX）
    popad

    ; 8. 丢弃 int_no + err_code
    add esp, 8

    ; 9. 返回用户态（iret 弹出 EIP/CS/EFLAGS/ESP/SS）
    iret
```

**关键点**：
- C handler `syscall_handler_int0x80(regs_t* r)` 将 `syscall_dispatch()` 的返回值
  写入 `r->eax`，这样 `popad` 恢复时用户态 EAX 自然为返回值。
- 使用 interrupt gate（CPU 进入时自动清 IF），保证 syscall 处理期间不被 IRQ 嵌套打断。
  如果将来需要允许中断嵌套（类似 Linux 的 sti-in-syscall 优化），可在 C handler 中
  手动 `sti`。

### C Backend 详细设计：`syscall_int0x80.c`

```c
// 伪代码
#include "idt.h"
#include "gdt.h"
#include "syscall.h"

extern void syscall_entry_int0x80(void);

// C handler — 由汇编 stub 调用
void syscall_handler_int0x80(regs_t* r) {
    syscall_regs_t regs = {
        .nr   = r->eax,
        .arg1 = r->ebx,
        .arg2 = r->ecx,
        .arg3 = r->edx,
        .arg4 = r->esi,
        .arg5 = r->edi,
    };
    r->eax = (uint32_t)syscall_dispatch(&regs);
}

// 后端 init — 安装 IDT gate 0x80
static void int0x80_init(void) {
    // 0xEE = P=1, DPL=3, Type=1110 (32-bit interrupt gate, 用户态可触发)
    idt_set_gate(0x80, syscall_entry_int0x80,
                 GDT_KERNEL_CODE_SEL, 0xEE);
    printk("[int0x80] IDT[0x80] installed (DPL=3 interrupt gate)\n");
}

static const syscall_ops_t int0x80_ops = {
    .name = "int0x80",
    .init = int0x80_init,
};

const syscall_ops_t* syscall_int0x80_get_ops(void) {
    return &int0x80_ops;
}
```

### 用户态测试代码设计：`user_test_code.asm` 新增片段

```nasm
; Ring 3 syscall 测试 — write("Hello from Ring 3!\n") + exit(0)
;
; 调用约定（Linux i386 ABI）：
;   EAX = syscall number
;   EBX = arg1, ECX = arg2, EDX = arg3
;
; 注意：字符串必须在用户代码页内（紧跟在代码指令后），
;   因为我们把整个 user_syscall_start..end 块复制到 0x00400000。
;   引用字符串时使用 $ - 相对寻址（或直接用标签，链接时是内核地址，
;   所以需要运行时重定位或使用 call/pop 取 EIP 的技巧）。
;
; [方案] 使用 call/pop 获取当前 EIP，再加偏移定位字符串：
;   call .next
;   .next: pop ebx        ; EBX = 运行时 .next 的地址
;   add ebx, (msg - .next); EBX = 运行时 msg 的地址

global user_syscall_start
global user_syscall_end

user_syscall_start:
    ; --- sys_write(1, "Hello from Ring 3!\n", 19) ---
    call .getpc
.getpc:
    pop ecx                  ; ECX = runtime address of .getpc
    add ecx, (.msg - .getpc) ; ECX = runtime address of .msg (= buf)
    mov eax, 4               ; SYS_WRITE
    mov ebx, 1               ; fd = stdout
    mov edx, 19              ; count
    int 0x80

    ; --- sys_exit(0) ---
    mov eax, 1               ; SYS_EXIT
    xor ebx, ebx             ; status = 0
    int 0x80

    ; 安全网（不应到达）
    jmp $

.msg:
    db "Hello from Ring 3!", 10   ; 10 = '\n', 共 19 字节
user_syscall_end:
```

**关键设计决策**：
- 字符串嵌入在代码块末尾，整体复制到用户地址空间。
- 使用 `call/pop` PIC（位置无关代码）技巧获取运行时地址，
  避免对绝对地址的依赖（代码会被复制到 0x00400000）。
- SYS_WRITE=4、SYS_EXIT=1 与 `syscall.h` 中的定义一致。

### cmd_ring3.c 改造

新增 `ring3_syscall()` 函数和 `ring3 syscall` 子命令：

1. 分配用户代码页 + 栈页（复用 `ring3_panic()` 的资源分配逻辑）
2. 复制 `user_syscall_start..user_syscall_end` 到 0x00400000
3. 检查 `syscall_is_ready()` 确认 int0x80 后端已初始化
4. 调用 `jump_to_ring3(USER_CODE_VADDR, USER_STACK_TOP)`
5. 用户态代码通过 int 0x80 调 sys_write → 串口输出 "Hello from Ring 3!\n"
6. 用户态代码通过 int 0x80 调 sys_exit(0) → 内核 halt

### kconfig

- `KCONFIG_SYSCALL_BACKEND = 0` 选择 int0x80（已定义，无需新增）

### Build

- **Makefile 无需修改** — `src/Makefile` 已使用 `$(wildcard kernel/*/*.c)` 和
  `$(wildcard kernel/*/*.asm)` 自动发现源文件，新增的 `syscall_stub.asm` 和
  `syscall_int0x80.c` 会自动参与编译。
- **compile_commands.json**：新增 .c 文件需要重新生成（`make compdb` 或 `AUTO_COMPDB=1`）。

## Book Plan (@Author)

### Chapter

- Target file: `book/chapters/ch13-syscall.tex`
- 需要新增的 section 文件（在 `book/chapters/ch13-syscall/` 目录下）：

| 序号 | 文件名 | 标题 |
|------|--------|------|
| sec11 | `sec11-int0x80-后端：IDT-gate-安装.tex` | int 0x80 后端：IDT gate 安装 |
| sec12 | `sec12-汇编-stub：寄存器保存与参数提取.tex` | 汇编 stub：寄存器保存与参数提取 |
| sec13 | `sec13-C-handler-与-dispatch-衔接.tex` | C handler 与 dispatch 衔接 |
| sec14 | `sec14-用户态封装宏.tex` | 用户态封装宏：_syscall0 ~ _syscall3 |
| sec15 | `sec15-端到端验证：Ring3-write-exit.tex` | 端到端验证：Ring 3 → write → exit |
| sec16 | `sec16-int0x80-小结.tex` | int 0x80 后端小结 |

在 `ch13-syscall.tex` 主文件中，在现有 `sec10` 之前（或之后根据叙事顺序调整）
添加 `\input` 引用。建议叙事顺序：sec01~sec09（D-2 概念与框架）→ sec11~sec16（D-3 int0x80 后端）→ sec10（小结与练习，更新为涵盖 D-3 内容）。

### Figures

| Figure | File | Content |
|--------|------|---------|
| `fig-int0x80-flow` | `book/figures/ch13/fig-int0x80-flow.tex` | TikZ 流程图：Ring 3 `int 0x80` → CPU 特权级切换 → 汇编 stub（寄存器保存、构建 syscall_regs_t）→ C handler → `syscall_dispatch()` → `sys_write()` → 返回链 → `iret` → Ring 3 |
| `fig-int0x80-stack` | `book/figures/ch13/fig-int0x80-stack.tex` | TikZ 栈帧图：int 0x80 触发后的完整栈布局（CPU 自动压栈部分 + stub 压栈部分），标注每个字段与 `regs_t` 结构体成员的对应关系 |
| `fig-syscall-arch` | `book/figures/ch13/fig-syscall-arch.tex` | TikZ 架构图：用户态宏 → int 0x80 → syscall_stub.asm → syscall_int0x80.c → syscall_dispatch → handler，标注 `syscall_ops_t` 可插拔边界 |

### Code Listings

- `\codefile{src/kernel/arch/syscall_stub.asm}` — 汇编 stub 完整代码
- `\codefile{src/kernel/arch/syscall_int0x80.c}` — C 后端初始化 + handler
- `\codefile{src/include/kernel/syscall_user.h}` — 用户态封装宏
- `\codefile{src/include/kernel/syscall_backends.h}` — 后端注册表
- `\codefile{src/kernel/arch/user_test_code.asm}` 节选 — user_syscall 片段
- `\codefile{src/kernel/cmds/cmd_ring3.c}` 节选 — ring3_syscall() 函数

## Verification

### Shell 命令测试

```
ring3 syscall
```

### Expected Output（串口 / QEMU -serial stdio）

```
[ring3] Preparing Ring 3 syscall test...
[ring3] code page: virt=0x00400000 phys=0x00XXXXXX
[ring3] stack page: virt=0x00401000 phys=0x00XXXXXX (top=0x00402000)
[ring3] test code copied (XX bytes): write+exit via int 0x80
[ring3] Jumping to Ring 3 (eip=0x00400000, esp=0x00402000)...
Hello from Ring 3!
[syscall] exit(status=0)
```

最后一行 `[syscall] exit(status=0)` 来自 `sys_exit()` 的 printk，之后系统 halt。

### 验证要点

1. **IDT gate 属性**：0x80 号向量 type_attr = 0xEE（DPL=3），确认用户态有权触发
2. **特权级切换**：int 0x80 时 CPL 3→0，TSS.ESP0 被加载为内核栈
3. **参数传递**：EAX=4(SYS_WRITE)、EBX=1(stdout)、ECX=buf、EDX=19 正确到达 handler
4. **返回值**：sys_write 返回 19，用户态 EAX 收到 19
5. **返回路径**：iret 正确恢复用户态 CS/SS/ESP/EIP/EFLAGS，继续执行 sys_exit
6. **安全性**：sys_write 中 `buf >= 0xC0000000` 检查仍然生效

### Regression Tests

执行以下命令确认现有功能不受影响：

- `pmm test` — 物理内存分配器自测
- `heap test` — kmalloc 堆自测
- `vma test` — VMA 子系统自测
- `ring3 panic` — 原 HLT GP-fault 测试仍工作

### GDB 调试验证（可选）

```gdb
# 在 IDT gate 安装处设断点
break int0x80_init
continue
# 确认 0x80 号 gate 已设置

# 在汇编入口设断点
break syscall_entry_int0x80
continue
# 执行 ring3 syscall 后应停在这里
# 检查 CS 是否来自 Ring 3：info registers → cs = 0x1b

# 在 C handler 设断点
break syscall_handler_int0x80
continue
# 检查 r->eax == 4 (SYS_WRITE), r->ebx == 1
```

## Dependencies

- **Requires**:
  - D-1 (TSS + Ring 3) ✅ — `jump_to_ring3()`、GDT User Code/Data、TSS.ESP0
  - D-2 (syscall_ops_t + dispatch) ✅ — `syscall_dispatch()`、`syscall_register_backend()`、`sys_write`/`sys_exit`/`sys_brk` handler

- **Blocks**:
  - D-4 (sysenter 后端) — 与 int0x80 并列的另一后端，验证 `KCONFIG_SYSCALL_BACKEND` 切换
  - D-5/D-6 (ELF loader) — 加载 ELF 后的第一件事是用 syscall 退出/输出
  - D-7/D-8 (进程管理) — `sys_exit` 需要真正的进程终止逻辑
  - F-9 (libc) — libc 的 syscall wrapper 将基于 `_syscall0~3` 宏或等价实现
