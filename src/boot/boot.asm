section .multiboot_header
align 8
header_start:
    dd 0xe85250d6                              ; Multiboot2 magic
    dd 0                                       ; architecture: i386
    dd header_end - header_start               ; header length
    dd 0x100000000 - (0xe85250d6 + 0 + (header_end - header_start)) ; checksum

    ; Required end tag (type=0, flags=0, size=8)
    dw 0
    dw 0
    dd 8

align 8
header_end:

section .text
bits 32
global _start
extern kmain
_start:
    ; 这里的代码会紧跟在上面那个 header 后面
    mov esp, stack_top
    and esp, 0xFFFFFFF0
    mov word [0xb8000], 0x2f4f ; O
    mov word [0xb8002], 0x2f4b ; K

    ; Multiboot2 passes:
    ;   EAX = 0x36d76289 (magic)
    ;   EBX = address of multiboot information structure
    push ebx
    push eax
    call kmain
    add esp, 8

    jmp .hlt

.hlt:
    hlt
    jmp .hlt

section .bss
align 4096
stack_bottom:
    resb 16384
stack_top: