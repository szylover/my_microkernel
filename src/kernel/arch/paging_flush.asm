; paging_flush.asm (NASM syntax)
;
; VMM 汇编辅助：操作 CR3/CR0 寄存器，开启/管理 x86 分页。
;
; ============================================================================
; [WHY] 为什么这些操作必须用汇编？
; ============================================================================
;
; CR0, CR3, CR4 是 x86 的控制寄存器 (Control Registers)，只能通过专用
; 的 `mov crN, reg` / `mov reg, crN` 指令访问。GCC 的内联汇编也能写，
; 但独立的 .asm 文件更清晰——你能精确看到每条指令对 CPU 状态的影响。
;
; ============================================================================
; [CPU STATE] CR0 关键位
; ============================================================================
;
;   bit 0  PE (Protection Enable) : 1 = 保护模式（GRUB 已开启）
;   bit 16 WP (Write Protect)     : 1 = Ring 0 也受 PTE Read-Only 限制
;   bit 31 PG (Paging)            : 1 = 开启分页
;
; 我们要做的就是：保持 PE=1 不变，把 PG 位从 0 设为 1。
;
; ============================================================================
; [CPU STATE] CR3 (Page Directory Base Register)
; ============================================================================
;
; CR3 存放当前页目录的 **物理地址**（高 20 位有效，低 12 位通常为 0）。
; 写入 CR3 会自动刷新整个 TLB（除了 Global 页）。
;

bits 32

global vmm_load_page_directory
global vmm_enable_paging
global vmm_invlpg

; ============================================================================
; void vmm_load_page_directory(uint32_t pd_phys_addr);
; ============================================================================
;
; [WHY]
;   把页目录的物理地址放入 CR3，这样 CPU 就知道从哪里开始查两级页表。
;   注意：此时 CR0.PG 可能还是 0（分页未开启），所以写入 CR3 只是"准备好"，
;   实际翻译要等 PG=1 才生效。
;
; [CPU STATE]
;   CR3 ← pd_phys_addr
;   副作用：如果分页已开启，会刷新整个 TLB
;
; cdecl: 参数在 [esp + 4]
;
vmm_load_page_directory:
    mov eax, [esp + 4]      ; eax = pd_phys_addr（从栈上取函数参数）
    mov cr3, eax             ; CR3 ← 页目录物理地址
    ret

; ============================================================================
; void vmm_enable_paging(void);
; ============================================================================
;
; [WHY]
;   设置 CR0 的 bit 31 (PG)，开启分页。
;   从执行 `mov cr0, eax` 之后的 **下一条指令** 起，CPU 的每次取指/数据
;   访问都会经过 MMU 翻译。
;
; [CPU STATE]
;   CR0.PG ← 1
;   此后所有内存访问都经过页表翻译。
;
; [CRITICAL]
;   调用前 **必须** 确保：
;   1. CR3 已指向有效的页目录
;   2. 当前 EIP 所在的物理地址已经做了 identity mapping
;   否则：CPU 取下一条指令 → Page Fault → Triple Fault
;
vmm_enable_paging:
    mov eax, cr0             ; 读出当前 CR0
    or  eax, 0x80000000      ; 设置 bit 31 (PG = Paging Enable)
    mov cr0, eax             ; 写回 CR0 → 分页立即生效！
    ret                      ; 返回到调用者（此 ret 已经在分页模式下执行）

; ============================================================================
; void vmm_invlpg(uint32_t virt_addr);
; ============================================================================
;
; [WHY]
;   当我们修改了一个 PTE（改映射 / 取消映射）后，CPU 的 TLB 里可能还
;   缓存着旧的翻译结果。`invlpg` 指令告诉 CPU：
;   "把这个虚拟地址的 TLB 缓存作废，下次访问重新查页表。"
;
;   如果不 invlpg：CPU 可能继续用旧映射访问错误的物理地址 → 数据损坏
;
; [CPU STATE]
;   使 virt_addr 所在页的 TLB 缓存条目失效。
;   不影响其他虚拟地址的 TLB 缓存。
;
; cdecl: 参数在 [esp + 4]
;
vmm_invlpg:
    mov eax, [esp + 4]      ; eax = virt_addr
    invlpg [eax]            ; 使该虚拟地址的 TLB 条目失效
    ret

; 标记栈不可执行（消除链接器警告）
section .note.GNU-stack noalloc noexec nowrite progbits
