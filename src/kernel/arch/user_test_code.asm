; user_test_code.asm (NASM)
;
; 用户态 Ring 3 测试代码片段。
;
; [WHY] 这些代码不会在内核里直接执行。
;   内核会把它们当作字节数组，复制到用户地址空间 (0x00400000)，
;   然后用 iret 跳转到 Ring 3 执行。
;
; 用标签标记每段代码的起止位置，C 通过 extern 引用来获取地址和大小。
;
; 使用方法（C 侧）：
;   extern const uint8_t user_panic_start[], user_panic_end[];
;   size_t size = user_panic_end - user_panic_start;
;   memcpy(dest, user_panic_start, size);

bits 32
section .rodata

; ============================================================================
; ring3 panic — 执行 HLT 触发 #GP
; ============================================================================
;
; HLT 是特权指令，Ring 3 无权执行 → CPU 触发 #GP (vector 13)。
; 后面的 jmp $ 是安全网（不应被执行到）。
;
global user_panic_start
global user_panic_end

user_panic_start:
    hlt                     ; Ring 3 执行此指令 → #GP
    jmp $                   ; 无穷循环（安全网，不应到达）
user_panic_end:

; ============================================================================
; ring3 syscall test — 通过 int 0x80 调用 write + exit
; ============================================================================
;
; [WHY] 验证完整的 Ring 3 → int 0x80 → syscall_dispatch → handler 路径。
;   write(1, "hello from ring3!\n", 19) 会通过 sys_write 输出到串口。
;   exit(0) 会通过 sys_exit 触发 hlt 让内核停机。
;
; [WHY] PIC（位置无关代码）技巧：
;   内核将此代码复制到用户地址空间 (USER_CODE_VADDR = 0x00400000)，
;   但汇编时我们不知道运行时地址。使用 call/pop 技巧获取运行时 EIP：
;
;     call sc_get_ip   ; 把 sc_get_ip 的地址压栈，然后跳转到 sc_get_ip
;   sc_get_ip:
;     pop esi          ; ESI = sc_get_ip 的运行时地址
;
;   然后用 [esi + (label - sc_get_ip)] 计算其他标签的运行时地址。
;   这样代码被复制到任意地址后仍能正确访问内联字符串。
;
; [CALLING CONVENTION] int 0x80 Linux i386 ABI：
;   EAX = syscall number (4=write, 1=exit)
;   EBX = arg1, ECX = arg2, EDX = arg3
;
global user_syscall_start
global user_syscall_end

user_syscall_start:
    ; --- PIC: 获取 sc_get_ip 的运行时地址 ---
    call sc_get_ip          ; push (address of sc_get_ip) onto stack, then jump
sc_get_ip:
    pop esi                 ; ESI = 运行时的 sc_get_ip 地址

    ; --- write(fd=1, buf=msg, count=msg_len) ---
    ; SYS_WRITE = 4 (Linux i386 ABI)
    mov eax, 4                                          ; syscall number: write
    mov ebx, 1                                          ; fd: stdout
    lea ecx, [esi + (sc_msg - sc_get_ip)]              ; buf: 运行时 msg 地址
    mov edx, sc_msg_len                                 ; count: 消息长度
    int 0x80                                            ; 触发 syscall

    ; --- exit(status=0) ---
    ; SYS_EXIT = 1 (Linux i386 ABI)
    mov eax, 1              ; syscall number: exit
    xor ebx, ebx            ; status: 0
    int 0x80                ; 触发 syscall

    jmp $                   ; 安全网（sys_exit 会 halt，不应到达这里）

sc_msg:     db "hello from ring3!", 0x0A    ; 18 字节（含换行符）
sc_msg_len  equ ($ - sc_msg)                ; 编译期计算长度
user_syscall_end:

section .note.GNU-stack noalloc noexec nowrite progbits
