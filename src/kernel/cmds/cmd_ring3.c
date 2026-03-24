/*
 * cmd_ring3.c — Ring 3（用户态）测试命令
 *
 * shell 命令：
 *   ring3 panic   — 跳转 Ring 3，执行 HLT → #GP（不可恢复）
 *   ring3 syscall — 跳转 Ring 3，通过 int 0x80 调用 write + exit
 *
 * [WHY] 验证 TSS + GDT 扩展 + iret 能正确地将 CPU 切换到 Ring 3。
 *   ring3 panic  测试：Ring 3 执行特权指令 (hlt) → 触发 #GP → 内核捕获并报告
 *   ring3 syscall测试：Ring 3 执行 int 0x80 → syscall_dispatch → sys_write/sys_exit
 */

#include "cmd.h"
#include "printk.h"
#include "tss.h"
#include "vmm.h"
#include "pmm.h"
#include "vma.h"

#include <stdint.h>

/*
 * 用户态地址配置
 *
 * [WHY] 选择 0x00400000 (4MiB) 作为用户代码起始地址：
 *   - 高于 NULL 保护区（方便检测空指针解引用）
 *   - 低于 0xC0000000（属于用户空间）
 *   - 这是 Linux 下 ELF 默认加载地址的附近值
 *
 * 用户栈放在代码页上方一页 (0x00401000)，栈顶在页末尾 (0x00402000)。
 */
#define USER_CODE_VADDR  0x00400000u
#define USER_STACK_VADDR 0x00401000u
#define USER_STACK_TOP   (USER_STACK_VADDR + VMM_PAGE_SIZE)

/*
 * 用户态 panic 测试代码（在 user_test_code.asm 中用汇编指令定义）
 *
 * [WHY] 用真正的汇编而非手写机器码，可读性更好：
 *   user_panic_start:
 *       hlt           ; Ring 3 执行此指令 → #GP
 *       jmp $         ; 安全网
 *   user_panic_end:
 *
 * C 通过 extern 引用这两个标签，计算大小并复制到用户地址空间。
 */
extern const uint8_t user_panic_start[];
extern const uint8_t user_panic_end[];

/*
 * 用户态 syscall 测试代码（在 user_test_code.asm 中用汇编指令定义）
 *
 * [WHY] 用 PIC（位置无关代码）技巧访问内联字符串，
 *   代码被复制到任意用户地址后仍能正确运行。
 *   详见 user_test_code.asm 中的注释。
 */
extern const uint8_t user_syscall_start[];
extern const uint8_t user_syscall_end[];

static void kmemcpy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static int ring3_panic(void) {
    printk("[ring3] Preparing Ring 3 panic test...\n");

    /* 1. 分配并映射用户代码页 */
    uint32_t code_phys = (uint32_t)(uintptr_t)pmm_alloc_page();
    if (code_phys == 0) {
        printk("[ring3] ERROR: failed to alloc code page\n");
        return -1;
    }

    /*
     * [WHY] PTE_USER 标志让用户态（Ring 3）可以访问这一页。
     *   vmm_map_page() 内部还会把 PDE 也设上 PDE_USER（Step 3c 的修复）。
     */
    if (vmm_map_page(USER_CODE_VADDR, code_phys,
                     PTE_PRESENT | PTE_USER) != 0) {
        printk("[ring3] ERROR: failed to map code page\n");
        return -1;
    }
    printk("[ring3] code page: virt=0x%08x phys=0x%08x\n",
           USER_CODE_VADDR, code_phys);

    /* 2. 分配并映射用户栈页 */
    uint32_t stack_phys = (uint32_t)(uintptr_t)pmm_alloc_page();
    if (stack_phys == 0) {
        printk("[ring3] ERROR: failed to alloc stack page\n");
        return -1;
    }

    /*
     * [WHY] 栈页需要 PTE_WRITABLE — 用户态 push/call 会写栈。
     */
    if (vmm_map_page(USER_STACK_VADDR, stack_phys,
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER) != 0) {
        printk("[ring3] ERROR: failed to map stack page\n");
        return -1;
    }
    printk("[ring3] stack page: virt=0x%08x phys=0x%08x (top=0x%08x)\n",
           USER_STACK_VADDR, stack_phys, USER_STACK_TOP);

    /* 3. 将测试汇编代码复制到用户代码页 */
    uint32_t code_size = (uint32_t)(user_panic_end - user_panic_start);
    kmemcpy((void*)USER_CODE_VADDR, user_panic_start, code_size);
    printk("[ring3] test code copied (%u bytes): HLT instruction\n", code_size);

    /* 4. 注册 VMA 区域（供 Page Fault handler 诊断） */
    if (vma_is_ready()) {
        vma_add(USER_CODE_VADDR, USER_CODE_VADDR + VMM_PAGE_SIZE,
                VMA_READ | VMA_EXEC, "user-code");
        vma_add(USER_STACK_VADDR, USER_STACK_TOP,
                VMA_READ | VMA_WRITE, "user-stack");
    }

    /*
     * 5. 跳转到 Ring 3 — 不返回！
     *
     * [CPU STATE] 重大状态变化：
     *   CPL: 0 → 3
     *   CS:  0x08 → 0x1B
     *   SS:  0x10 → 0x23
     *   EIP: → 0x00400000 (HLT 指令所在地址)
     *   ESP: → 0x00402000 (用户栈顶)
     *
     * 用户代码执行 HLT → Ring 3 无权执行特权指令 → #GP → ISR 报告
     */
    printk("[ring3] Jumping to Ring 3 (eip=0x%08x, esp=0x%08x)...\n",
           USER_CODE_VADDR, USER_STACK_TOP);
    printk("[ring3] Expected: #GP fault (HLT is privileged)\n");

    jump_to_ring3(USER_CODE_VADDR, USER_STACK_TOP);

    /* 不应到达 */
    return -1;
}

/*
 * ring3_syscall — 跳转到 Ring 3 并通过 int 0x80 执行 write + exit
 *
 * [WHY] 这是 D-3 的端到端测试：
 *   Ring 3 代码 → int 0x80 → syscall_int80_entry (ASM) →
 *   syscall_dispatch (C) → sys_write / sys_exit (handler)
 *
 *   成功标志：
 *   1. 串口输出 "hello from ring3!" 一行
 *   2. sys_exit 打印 "[syscall] exit(status=0)" 后 halt
 */
static int ring3_syscall(void) {
    printk("[ring3] Preparing Ring 3 syscall test...\n");

    /* 1. 分配并映射用户代码页 */
    uint32_t code_phys = (uint32_t)(uintptr_t)pmm_alloc_page();
    if (code_phys == 0) {
        printk("[ring3] ERROR: failed to alloc code page\n");
        return -1;
    }

    /*
     * [WHY] 代码页只需 PTE_USER，不需要 PTE_WRITABLE。
     *   PIC 技巧里的 `call sc_get_ip` 会把返回地址压到用户栈（0x00401000 页），
     *   而非代码页（0x00400000）——栈页已设 PTE_WRITABLE，代码页本身无需写权限。
     *   x86 32-bit 非 PAE 模式没有独立的执行位，可读即可执行。
     */
    if (vmm_map_page(USER_CODE_VADDR, code_phys,
                     PTE_PRESENT | PTE_USER) != 0) {
        printk("[ring3] ERROR: failed to map code page\n");
        return -1;
    }
    printk("[ring3] code page: virt=0x%08x phys=0x%08x\n",
           USER_CODE_VADDR, code_phys);

    /* 2. 分配并映射用户栈页 */
    uint32_t stack_phys = (uint32_t)(uintptr_t)pmm_alloc_page();
    if (stack_phys == 0) {
        printk("[ring3] ERROR: failed to alloc stack page\n");
        return -1;
    }

    if (vmm_map_page(USER_STACK_VADDR, stack_phys,
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER) != 0) {
        printk("[ring3] ERROR: failed to map stack page\n");
        return -1;
    }
    printk("[ring3] stack page: virt=0x%08x phys=0x%08x (top=0x%08x)\n",
           USER_STACK_VADDR, stack_phys, USER_STACK_TOP);

    /* 3. 复制 syscall 测试代码到用户代码页 */
    uint32_t code_size = (uint32_t)(user_syscall_end - user_syscall_start);
    kmemcpy((void*)USER_CODE_VADDR, user_syscall_start, code_size);
    printk("[ring3] syscall test code copied (%u bytes)\n", code_size);

    /* 4. 注册 VMA 区域（供 #PF handler 诊断） */
    if (vma_is_ready()) {
        vma_add(USER_CODE_VADDR, USER_CODE_VADDR + VMM_PAGE_SIZE,
                VMA_READ | VMA_EXEC, "user-code");
        vma_add(USER_STACK_VADDR, USER_STACK_TOP,
                VMA_READ | VMA_WRITE, "user-stack");
    }

    /*
     * 5. 跳转到 Ring 3 — 不返回！
     *
     * [CPU STATE]
     *   CPL: 0 → 3
     *   CS:  0x08 → 0x1B  (GDT_USER_CODE_SEL)
     *   SS:  0x10 → 0x23  (GDT_USER_DATA_SEL)
     *   EIP: → USER_CODE_VADDR (syscall test code 入口)
     *   ESP: → USER_STACK_TOP  (用户栈顶)
     *
     * 执行流程：
     *   1. 用户代码 call sc_get_ip → PIC 获取当前 EIP
     *   2. write(1, "hello from ring3!\n", 18) via int 0x80
     *      → sys_write → printk 输出到串口
     *   3. exit(0) via int 0x80
     *      → sys_exit → hlt（内核停机）
     */
    printk("[ring3] Jumping to Ring 3 (syscall test)...\n");
    printk("[ring3] Expected: write -> 'hello from ring3!' on serial, "
           "then exit -> halt\n");

    jump_to_ring3(USER_CODE_VADDR, USER_STACK_TOP);

    /* 不应到达 */
    return -1;
}

static int streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int cmd_ring3_fn(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: ring3 <subcommand>\n");
        printk("  ring3 panic   - jump to Ring 3, execute HLT -> #GP (destructive)\n");
        printk("  ring3 syscall - jump to Ring 3, test int 0x80: write + exit\n");
        return 0;
    }

    const char* sub = argv[1];
    if (streq(sub, "panic")) {
        return ring3_panic();
    }
    if (streq(sub, "syscall")) {
        return ring3_syscall();
    }

    printk("Unknown subcommand: %s\n", sub);
    return -1;
}

const cmd_t cmd_ring3 = {
    .name = "ring3",
    .help = "Ring 3 (user mode) tests: panic | syscall",
    .fn   = cmd_ring3_fn,
};
