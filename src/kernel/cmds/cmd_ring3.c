/*
 * cmd_ring3.c — Ring 3（用户态）测试命令
 *
 * shell 命令：ring3 test
 *
 * [WHY] 验证 TSS + GDT 扩展 + iret 能正确地将 CPU 切换到 Ring 3。
 *   测试方案：
 *   - 在用户地址空间分配一页代码 + 一页栈
 *   - 复制一小段"用户态测试代码"到代码页
 *   - 用 iret 跳转到 Ring 3
 *   - 用户态代码执行特权指令 (hlt) → 触发 #GP → 内核捕获并报告
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
 * 用户态测试代码（机器码）
 *
 * 机器码等价汇编：
 *   0xF4  ; hlt — Ring 3 执行此指令会触发 #GP
 *   0xEB  ; jmp rel8
 *   0xFE  ; -2 (infinite loop fallback, 不应到达)
 */
static const uint8_t user_test_code[] = {
    0xF4,             /* hlt — Ring 3 执行此指令触发 #GP */
    0xEB, 0xFE        /* jmp $ — 无穷循环（安全网） */
};

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

    /* 3. 将测试机器码复制到用户代码页 */
    kmemcpy((void*)USER_CODE_VADDR, user_test_code, sizeof(user_test_code));
    printk("[ring3] test code copied (%u bytes): HLT instruction\n",
           (unsigned)sizeof(user_test_code));

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
        printk("  ring3 panic  - jump to Ring 3, execute HLT -> #GP (destructive)\n");
        return 0;
    }

    const char* sub = argv[1];
    if (streq(sub, "panic")) {
        return ring3_panic();
    }

    printk("Unknown subcommand: %s\n", sub);
    return -1;
}

const cmd_t cmd_ring3 = {
    .name = "ring3",
    .help = "Ring 3 (user mode) tests",
    .fn   = cmd_ring3_fn,
};
