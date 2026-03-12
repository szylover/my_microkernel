; ============================================================================
; Multiboot2 Header
; ============================================================================
;
; [WHY] GRUB scans the first 32 KiB of the OS image for this header.
; It must appear early in the file, so the linker puts .multiboot_header
; at the start of the .boot section (physical address).
;
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

; ============================================================================
; Boot Page Directory (BSS at physical address)
; ============================================================================
;
; [WHY] This temporary page directory is used only during the boot→high-half
; transition. vmm_init() will replace it with a proper 4KiB-page setup.
; It must live at a physical address because we fill it before paging is on.
;
section .boot_bss nobits alloc noexec write align=4096
boot_pd:
    resb 4096

; ============================================================================
; Early Boot Code (before paging — at physical address)
; ============================================================================
;
; [CPU STATE] When GRUB hands control to _start:
;   EAX = 0x36d76289 (Multiboot2 magic)
;   EBX = physical address of Multiboot2 info structure
;   CR0.PG = 0 (paging OFF)
;   EIP is at physical address ~0x00100000
;
; Our job:
;   1. Build a temporary page directory with 4 MiB PSE pages
;   2. Map both identity (0→0) and high-half (0xC0000000→0) so:
;      - current EIP keeps working after enabling paging (identity)
;      - we can jump to kernel code linked at 0xC01xxxxx (high-half)
;   3. Enable PSE + paging
;   4. Jump to _start_high (virtual address 0xC01xxxxx)
;
section .boot
bits 32
global _start

KERNEL_VIRT_BASE equ 0xC0000000

_start:
    ; Save multiboot2 info in callee-saved registers
    mov esi, eax            ; magic
    mov edi, ebx            ; mb2 info pointer (physical address)

    ; VGA debug marker — visible even if serial isn't working yet
    mov word [0xb8000], 0x2f48 ; 'H' (for High-half boot)
    mov word [0xb8002], 0x2f48 ; 'H'

    ; ------------------------------------------------------------------
    ; Build temporary boot page directory (4 MiB PSE pages)
    ; ------------------------------------------------------------------
    ;
    ; [WHY] Using 4 MiB pages (PSE) instead of 4 KiB:
    ;   Only need a page directory — no page tables to allocate.
    ;   Simpler for boot, vmm_init() will replace with 4 KiB pages.
    ;
    ; PSE PDE format: [31:22]=phys_4MiB_base | [7]=PS=1 | [1]=RW | [0]=P
    ;   0x83 = PS(bit7) | RW(bit1) | Present(bit0)
    ;
    ; We map first 16 MiB (4 × 4 MiB pages) for both:
    ;   PD[0..3]     → identity:  virt 0x00000000 → phys 0x00000000
    ;   PD[768..771] → high-half: virt 0xC0000000 → phys 0x00000000
    ;
    ; [WHY] boot PSE 映射范围
    ;
    ; 必须覆盖 PMM 管理的所有物理内存，否则 vmm_init 分配页表时
    ; 通过 PHYS_TO_VIRT 访问高地址页会 #PF。
    ;
    ; 映射 1GiB（256 个 4MiB PSE 页），这是 high-half 内核虚拟地址空间
    ; 的上限（0xC0000000 - 0xFFFFFFFF）。超出实际 RAM 的 PDE 不会被访问，
    ; vmm_init() 最终会用 4KiB 页表替换这些 PSE 条目。

    ; Clear boot_pd (1024 entries × 4 bytes = 4 KiB)
    push edi                ; save mb2 pointer
    mov ecx, 1024
    xor eax, eax
    mov edi, boot_pd
    rep stosd
    pop edi                 ; restore mb2 pointer

    ; Build identity + high-half PSE mapping: 256 × 4 MiB = 1 GiB
    ;   PD[i]     → phys i*4MiB  (identity: virt 0 → phys 0)
    ;   PD[768+i] → phys i*4MiB  (high-half: virt 0xC0000000 → phys 0)
    ;
    ; PSE PDE format: [31:22]=phys_4MiB_base | [7]=PS=1 | [1]=RW | [0]=P
    ;   flags = 0x83 = PS(bit7) | RW(bit1) | Present(bit0)
    xor ecx, ecx            ; ecx = loop counter (0..255)
.fill_pse:
    mov eax, ecx
    shl eax, 22             ; eax = ecx * 4 MiB (physical base)
    or  eax, 0x83           ; flags: PS | RW | Present
    mov [boot_pd + ecx*4],       eax   ; identity: PD[i]
    mov [boot_pd + 768*4 + ecx*4], eax ; high-half: PD[768+i]
    inc ecx
    cmp ecx, 256
    jb  .fill_pse

    ; ------------------------------------------------------------------
    ; Enable PSE (Page Size Extension) in CR4
    ; ------------------------------------------------------------------
    ;
    ; [CPU STATE] CR4.PSE (bit 4) = 1
    ;   Allows 4 MiB pages when PDE has PS bit (bit 7) set.
    ;
    mov eax, cr4
    or  eax, 0x00000010     ; CR4.PSE = 1
    mov cr4, eax

    ; ------------------------------------------------------------------
    ; Load CR3 and enable paging
    ; ------------------------------------------------------------------
    ;
    ; [CPU STATE] CR3 ← boot_pd (physical address)
    ;
    mov eax, boot_pd
    mov cr3, eax

    ;
    ; [CPU STATE] CR0.PG (bit 31) ← 1
    ;   From the NEXT instruction, all memory accesses go through the MMU.
    ;   Identity mapping (PD[0..3]) ensures the current EIP still works.
    ;
    ; [CRITICAL] If identity mapping is wrong, the CPU can't fetch the
    ;   next instruction → #PF → #DF → Triple Fault → reboot
    ;
    mov eax, cr0
    or  eax, 0x80000000     ; CR0.PG = 1
    mov cr0, eax

    ; --- Paging is now ON ---

    ; Jump to high virtual address.
    ;
    ; [WHY] _start_high is in .text, linked at virtual 0xC01xxxxx.
    ; `mov eax, _start_high` loads this high virtual address.
    ; `jmp eax` jumps there — the high-half mapping (PD[768..771])
    ; translates it to the correct physical address.
    ;
    mov eax, _start_high
    jmp eax


; ============================================================================
; High-Half Entry Point (in .text — linked at virtual 0xC01xxxxx)
; ============================================================================
;
; [CPU STATE] Now executing at virtual address 0xC01xxxxx.
;   High-half mapping: PD[768] translates 0xC01xxxxx → phys 0x001xxxxx.
;   Identity mapping still present (PD[0..3]) — needed for VGA, mb2 info, etc.
;
section .text
bits 32
global _start_high
extern kmain

_start_high:
    ; Set up kernel stack (stack_top is at virtual address 0xC0xxxxxx)
    mov esp, stack_top
    and esp, 0xFFFFFFF0

    ; Call kmain(mb2_magic, mb2_info)
    ;
    ; [WHY] mb2_info (in edi) is still a physical address from GRUB.
    ; With identity mapping present, it can be dereferenced directly.
    ; vmm_init() keeps identity mapping for now.
    ;
    push edi                ; mb2_info pointer (physical address)
    push esi                ; mb2_magic
    call kmain
    add esp, 8

.hlt:
    hlt
    jmp .hlt

; ============================================================================
; Kernel Stack (in .bss — linked at virtual address 0xC0xxxxxx)
; ============================================================================
section .bss
align 4096
stack_bottom:
    resb 16384
stack_top:

; Mark stack as non-executable (silences ld warning about missing .note.GNU-stack)
section .note.GNU-stack noalloc noexec nowrite progbits
