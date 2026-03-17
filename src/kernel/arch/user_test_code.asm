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

; 将来可以在这里添加更多测试代码片段，例如：
;
; global user_loop_start
; global user_loop_end
; user_loop_start:
;     jmp $                 ; 无穷循环（测试中断能否打断 Ring 3）
; user_loop_end:

section .note.GNU-stack noalloc noexec nowrite progbits
