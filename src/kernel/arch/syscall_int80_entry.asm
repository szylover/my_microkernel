; syscall_int80_entry.asm — int 0x80 系统调用汇编入口（D-3）
;
; ============================================================================
; [CALLING CONVENTION] int 0x80 寄存器约定（与 Linux i386 ABI 一致）
; ============================================================================
;
;   EAX = syscall number（系统调用号）
;   EBX = arg1（第 1 参数）
;   ECX = arg2（第 2 参数）
;   EDX = arg3（第 3 参数）
;   ESI = arg4（第 4 参数）
;   EDI = arg5（第 5 参数）
;   返回值通过 EAX 传回用户态
;
; ============================================================================
; [STACK AT ENTRY] Ring 3 → Ring 0 跨特权级切换时 CPU 自动压入的内容
; ============================================================================
;
;   [ESP+0 ] EIP_user   — 用户态返回地址（int 0x80 后的下一条指令）
;   [ESP+4 ] CS_user    — 用户态代码段选择子 (RPL=3, 即 0x1B)
;   [ESP+8 ] EFLAGS     — 用户态标志寄存器快照
;   [ESP+12] ESP_user   — 用户态栈指针（跨特权级时才存在）
;   [ESP+16] SS_user    — 用户态栈段选择子（跨特权级时才存在）
;
; [WHY] 只有跨特权级（CPL 变化）时 CPU 才会压入 ESP_user/SS_user。
;   这里 Ring3→Ring0 必然跨特权级，所以完整的 5 字段帧总是存在。
;
; ============================================================================
; [CPU STATE] 进入 handler 时的状态
; ============================================================================
;
;   CPL = 0（内核态）        — CPU 已切换特权级
;   SS:ESP = TSS.SS0:ESP0   — CPU 已切换到内核栈（来自 TSS）
;   IF = 0                  — interrupt gate 会自动清 IF（关中断）
;   CS = GDT_KERNEL_CODE_SEL（来自 IDT gate descriptor 中的 selector 字段）
;
; ============================================================================

bits 32

extern syscall_dispatch

global syscall_int80_entry

; ============================================================================
; syscall_int80_entry — int 0x80 的汇编入口
; ============================================================================
syscall_int80_entry:
    ; -------------------------------------------------------------------------
    ; 步骤 1：在栈上构造 syscall_regs_t 结构体
    ;
    ; syscall_regs_t 布局（见 syscall.h）：
    ;   offset 0  : nr   (uint32_t) ← EAX
    ;   offset 4  : arg1 (uint32_t) ← EBX
    ;   offset 8  : arg2 (uint32_t) ← ECX
    ;   offset 12 : arg3 (uint32_t) ← EDX
    ;   offset 16 : arg4 (uint32_t) ← ESI
    ;   offset 20 : arg5 (uint32_t) ← EDI
    ;
    ; [WHY] 以逆序压栈（arg5 先压），使 ESP 指向结构体起始后
    ;   ESP+0 = nr, ESP+4 = arg1, ... ESP+20 = arg5
    ;   这正好与 syscall_regs_t 的内存布局吻合。
    ; -------------------------------------------------------------------------
    push edi        ; arg5  (offset 20 from struct base, pushed first)
    push esi        ; arg4  (offset 16)
    push edx        ; arg3  (offset 12)
    push ecx        ; arg2  (offset  8)
    push ebx        ; arg1  (offset  4)
    push eax        ; nr    (offset  0) ← ESP 现在指向结构体

    ; -------------------------------------------------------------------------
    ; 步骤 2：保存段寄存器，切换到内核数据段
    ;
    ; [WHY] int 0x80 触发时，CPU 把 CS 换成了内核代码段（来自 IDT gate），
    ;   但 DS/ES/FS/GS 仍然是用户态的值（0x23）。
    ;   在 C 函数里访问内核全局变量（如 syscall_table[]）之前，
    ;   必须把数据段切换到内核数据段 0x10，否则段基址不对。
    ;
    ; [CPU STATE] 切换 DS/ES/FS/GS → GDT_KERNEL_DATA_SEL (0x10)
    ;   内核代码可以安全地访问整个物理地址空间（flat model）。
    ; -------------------------------------------------------------------------
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10    ; GDT_KERNEL_DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; -------------------------------------------------------------------------
    ; 步骤 3：调用 syscall_dispatch(&regs)
    ;
    ; 栈布局此刻（从 ESP 向高地址）：
    ;   [ESP+ 0] gs
    ;   [ESP+ 4] fs
    ;   [ESP+ 8] es
    ;   [ESP+12] ds
    ;   [ESP+16] nr      ← syscall_regs_t 的起始地址
    ;   [ESP+20] arg1
    ;   ...
    ;
    ; 所以 &regs = ESP + 16（4 个段寄存器 × 4 字节）。
    ;
    ; [WHY] 用 lea 取地址而不是直接 push esp，是因为 esp 此刻指向 gs，
    ;   不是 syscall_regs_t 的起始。lea 加上偏移量得到正确地址。
    ; -------------------------------------------------------------------------
    lea eax, [esp + 16]
    push eax                ; 传参：syscall_regs_t*
    call syscall_dispatch
    add esp, 4              ; 清理 syscall_dispatch 的参数

    ; EAX 现在持有 syscall_dispatch() 的返回值（即 handler 的返回值）。

    ; -------------------------------------------------------------------------
    ; 步骤 4：恢复段寄存器
    ;
    ; [CPU STATE] DS/ES/FS/GS 恢复为用户态值（0x23）。
    ;   iret 返回用户态时 CS/SS 会被 CPU 自动恢复，
    ;   但 DS/ES/FS/GS 需要我们手动恢复——否则用户态代码访问数据会出错。
    ; -------------------------------------------------------------------------
    pop gs
    pop fs
    pop es
    pop ds

    ; -------------------------------------------------------------------------
    ; 步骤 5：恢复用户寄存器，把返回值放入 EAX，然后 iret
    ;
    ; 栈上当前 syscall_regs_t（去掉段寄存器后）：
    ;   [ESP+ 0] nr    ← 要被 syscall 返回值覆盖
    ;   [ESP+ 4] arg1  → EBX
    ;   [ESP+ 8] arg2  → ECX
    ;   [ESP+12] arg3  → EDX
    ;   [ESP+16] arg4  → ESI
    ;   [ESP+20] arg5  → EDI
    ;
    ; 之后 iret 会从栈上弹出：EIP_user, CS_user, EFLAGS, ESP_user, SS_user
    ; -------------------------------------------------------------------------
    mov [esp], eax  ; 把 syscall 返回值写到 nr 槽位（将被 pop eax 取出）

    pop eax         ; EAX = syscall 返回值（用户态通过 EAX 接收结果）
    pop ebx         ; 恢复用户态 EBX（arg1 原值）
    pop ecx         ; 恢复用户态 ECX（arg2 原值）
    pop edx         ; 恢复用户态 EDX（arg3 原值）
    pop esi         ; 恢复用户态 ESI（arg4 原值）
    pop edi         ; 恢复用户态 EDI（arg5 原值）

    ; -------------------------------------------------------------------------
    ; 步骤 6：iret — 从内核态返回用户态
    ;
    ; [CPU STATE] iret 执行后：
    ;   EIP  ← EIP_user（int 0x80 之后的下一条指令）
    ;   CS   ← CS_user  (0x1B, DPL=3)  → CPL 变回 3
    ;   EFLAGS ← 用户态 EFLAGS（IF 等标志恢复）
    ;   ESP  ← ESP_user（用户态栈指针）
    ;   SS   ← SS_user  (0x23, DPL=3)
    ;
    ; [WHY] 必须是 iret 而非 ret：
    ;   跨特权级的返回必须通过 iret，它会同时恢复 CS/EFLAGS/SS/ESP，
    ;   并触发 CPL 从 0 回到 3 的切换。普通 ret 只弹 EIP，无法完成特权级回退。
    ; -------------------------------------------------------------------------
    iret

; Mark stack as non-executable (silences GNU ld warning)
section .note.GNU-stack noalloc noexec nowrite progbits
