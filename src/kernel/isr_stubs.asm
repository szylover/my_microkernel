; isr_stubs.asm (NASM)
;
; 目标：为 CPU 异常向量 0..31 提供统一的汇编入口（ISR stub），
;       保存寄存器、规范化 error code，然后调用 C 函数 isr_handler_c。
;
; 学习级要点：
; 1) 为什么需要 stub：
;    - CPU 自动压栈的内容有限（eip/cs/eflags + 某些异常的 error code）。
;    - 我们希望对所有异常统一成同一种栈布局，方便 C 侧解析。
; 2) 哪些异常自带 error code：
;    - 8, 10, 11, 12, 13, 14, 17（#DF #TS #NP #SS #GP #PF #AC）
;    这些异常发生时，CPU 会额外压入一个 error code。
; 3) 统一布局的方法：
;    - 对“不带 error code”的异常：我们手动 push 0
;    - 对“带 error code”的异常：不额外 push 0
;    - 然后所有异常都 push 向量号 int_no
;    -> 这样在进入公共处理时，栈顶总是 int_no，其下是 err_code
;
; 注意：我们使用 interrupt gate 时，CPU 进入 handler 会清 IF。
; 这能避免早期嵌套中断把栈打爆（后面做 IRQ 再精细化）。

bits 32

extern isr_handler_c

global isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
global isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
global isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
global isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

%macro ISR_NOERR 1
isr%1:
    push dword 0          ; err_code = 0 (人为补齐)
    push dword %1         ; int_no
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
isr%1:
    push dword %1         ; int_no（CPU 已经压入 err_code）
    jmp isr_common_stub
%endmacro

; 0..7: no error code
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7

; 8: #DF has error code
ISR_ERR 8

; 9: no error code
ISR_NOERR 9

; 10..14: error code
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14

; 15..16: no error code
ISR_NOERR 15
ISR_NOERR 16

; 17: #AC has error code
ISR_ERR 17

; 18..31: no error code
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; 公共入口：保存寄存器并调用 C
isr_common_stub:
    ; 保存通用寄存器
    pushad

    ; 保存段寄存器（顺序要和 C 结构体匹配：gs,fs,es,ds）
    push ds
    push es
    push fs
    push gs

    ; 切换到内核数据段（保证 C 代码访问内存安全）
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 传参：把当前栈顶指针作为 regs_t* 传给 C
    push esp
    call isr_handler_c
    add esp, 4

    ; 恢复段寄存器
    pop gs
    pop fs
    pop es
    pop ds

    ; 恢复通用寄存器
    popad

    ; 丢弃 int_no + err_code（共 8 字节）
    add esp, 8

    iret
