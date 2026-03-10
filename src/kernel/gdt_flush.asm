; gdt_flush.asm (NASM syntax)
;
; 学习级说明：
; - lgdt 只是装载 GDTR，不会自动刷新段寄存器缓存。
; - CS 的刷新必须用 far jump/far call/iret 之一。
; - 其他段寄存器（DS/ES/SS/FS/GS）必须显式 mov 选择子。
;
; cdecl 调用约定：
;   void gdt_flush(const gdtr_t* gdtr_ptr);
; 参数在 [esp + 4]
;
; 选择子约定（与 include/gdt.h 对应）：
;   0x08 = kernel code selector
;   0x10 = kernel data selector

bits 32

global gdt_flush

gdt_flush:
    mov eax, [esp + 4]      ; eax = gdtr_ptr
    lgdt [eax]              ; load GDTR

    ; Far jump: reload CS (and flush the hidden part of CS)
    jmp 0x08:.flush_cs

.flush_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; SS reload has a special rule: interrupts are inhibited until after
    ; the next instruction. We insert a harmless instruction before ret.
    mov ss, ax
    mov esp, esp

    ret
