; irq_stubs.asm (NASM)
;
; 目标：为 PIC 产生的硬件中断 IRQ0..IRQ15 提供统一的汇编入口。
;
; 关键点：
; - 我们把 PIC remap 到：
;     master IRQ0..7  -> vector 0x20..0x27
;     slave  IRQ8..15 -> vector 0x28..0x2F
; - 硬件 IRQ 不会像某些 CPU 异常那样自动压入 error code。
;   为了让 C 侧的寄存器布局与异常处理一致，我们人为 push 一个 0 作为 err_code。
; - 栈布局与 `src/kernel/idt.c` 中的 regs 结构保持一致：
;     [gs fs es ds] [pushad regs] [int_no err_code] [eip cs eflags]
;
; 后续扩展：
; - 当你要做更复杂的驱动时，可以在 C 层根据 irq 号分发到具体 handler。

bits 32

extern irq_handler_c

global irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
global irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

%macro IRQ 2
irq%1:
    push dword 0          ; err_code = 0 (硬件 IRQ 无 error code)
    push dword %2         ; int_no = vector (0x20 + irq)
    jmp irq_common_stub
%endmacro

; master PIC (IRQ0..7) -> 0x20..0x27
IRQ 0, 0x20
IRQ 1, 0x21
IRQ 2, 0x22
IRQ 3, 0x23
IRQ 4, 0x24
IRQ 5, 0x25
IRQ 6, 0x26
IRQ 7, 0x27

; slave PIC (IRQ8..15) -> 0x28..0x2F
IRQ 8,  0x28
IRQ 9,  0x29
IRQ 10, 0x2A
IRQ 11, 0x2B
IRQ 12, 0x2C
IRQ 13, 0x2D
IRQ 14, 0x2E
IRQ 15, 0x2F

irq_common_stub:
    ; 保存通用寄存器
    pushad

    ; 保存段寄存器（顺序要和 C 结构体匹配：gs,fs,es,ds）
    push ds
    push es
    push fs
    push gs

    ; 切换到内核数据段
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 传参：regs_t*（当前栈顶）
    push esp
    call irq_handler_c
    add esp, 4

    ; 恢复段寄存器
    pop gs
    pop fs
    pop es
    pop ds

    ; 恢复通用寄存器
    popad

    ; 丢弃 int_no + err_code
    add esp, 8

    iret

section .note.GNU-stack noalloc noexec nowrite progbits
