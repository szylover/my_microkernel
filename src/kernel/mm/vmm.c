#include <stdint.h>
#include <stddef.h>

#include "vmm.h"
#include "pmm.h"
#include "vma.h"
#include "printk.h"

/*
 * Virtual Memory Manager (VMM) — x86 32-bit 两级页表 (High-Half Kernel)
 *
 * ============================================================================
 * [WHY] High-Half Kernel 架构
 * ============================================================================
 *
 * 内核链接在虚拟地址 0xC0000000+ (高半区)，物理加载在 0x00100000。
 * 地址空间划分：
 *   0x00000000 - 0xBFFFFFFF : 用户态（将来给进程用）
 *   0xC0000000 - 0xFFFFFFFF : 内核态
 *
 * boot.asm 已经用 4MiB PSE 页建立了临时映射（identity + high-half）。
 * vmm_init() 会用正式的 4KiB 页表替换临时映射。
 *
 * [CRITICAL] 物理地址不能直接当指针用！
 *   必须通过 PHYS_TO_VIRT() 转换为虚拟地址后才能解引用。
 *   PDE/PTE 中存储的始终是物理地址（给 CPU/MMU 用）。
 */

/* Linker 导出的内核边界（定义在 linker.ld） */
extern char __kernel_phys_start[];  /* 物理起始地址 (0x00100000) */
extern char __kernel_phys_end[];    /* 物理结束地址 */
extern char __kernel_virt_start[];  /* 虚拟起始地址 (0xC01xxxxx) */
extern char __kernel_virt_end[];    /* 虚拟结束地址 */

/* ============================================================================
 * 汇编辅助函数（定义在 paging_flush.asm）
 * ============================================================================
 *
 * [WHY] 为什么用汇编？
 *   CR3 和 CR0 是特权寄存器，只能用 `mov cr3, eax` 等指令访问。
 *   虽然 GCC 内联汇编也可以，但独立的 .asm 文件更清晰、更易调试。
 */
extern void vmm_load_page_directory(uint32_t pd_phys_addr);
extern void vmm_enable_paging(void);
extern void vmm_invlpg(uint32_t virt_addr);

/* ============================================================================
 * 内部状态
 * ============================================================================ */

/*
 * g_kernel_pd — 内核页目录
 *
 * [WHY] 用 static 全局变量而非 pmm_alloc_page()？
 *   页目录必须 4KiB 对齐。用 __attribute__((aligned)) 声明的静态变量
 *   由链接器保证对齐，且在 BSS 段自动清零，简单可靠。
 *   （将来给用户进程创建页目录时，才需要动态分配。）
 */
static page_directory_t g_kernel_pd __attribute__((aligned(VMM_PAGE_SIZE)));

/*
 * g_vmm_ready — 分页是否已开启
 *
 * [WHY] 防止在初始化前误调用 vmm_map_page 等函数。
 */
static int g_vmm_ready = 0;

/*
 * g_next_vaddr — 下一个可分配的虚拟地址（Bump Allocator）
 *
 * [WHY] vmm_alloc_pages() 需要知道"哪些虚拟地址还没被用"。
 *   最简单的方案是 bump allocator（推进式分配）：
 *   - 维护一个"水位线"指针，初始值设在直接映射区的上方
 *   - 每次分配 N 页，就把水位线往上推 N × 4KB
 *   - 不回收虚拟地址空间（free 时只释放物理页，不回退水位线）
 *
 *   这很粗暴，但对内核堆来说足够了：
 *   - 内核虚拟空间有 1GB（0xC0000000 − 0xFFFFFFFF）
 *   - 直接映射区占了前面一部分（取决于物理内存大小）
 *   - 剩余空间给 vmm_alloc_pages 用，通常有几百 MB
 *
 *   将来 Stage 9 (VMA) 会用红黑树替代这个粗暴的 bump allocator。
 *
 * 初始值 0 表示"还没计算"，在 vmm_init() 结束时会设置为
 * 直接映射区上方（向上对齐到 4MiB 边界，留出安全间隔）。
 */
static uint32_t g_next_vaddr = 0;

/*
 * g_direct_map_end — 直接映射区结束的虚拟地址
 *
 * [WHY] VMA 子系统注册 "direct-map" 区域时需要知道边界。
 *   在 vmm_init() 末尾设置为 KERNEL_VIRT_OFFSET + map_end。
 */
static uint32_t g_direct_map_end = 0;

/* ============================================================================
 * 内部辅助函数
 * ============================================================================ */

/*
 * 从虚拟地址中提取 Page Directory index（高 10 位）
 *
 * 虚拟地址布局回顾：
 *   [31:22] PD index | [21:12] PT index | [11:0] offset
 */
static inline uint32_t pd_index(uint32_t virt) {
    return (virt >> 22) & 0x3FFu;
}

/*
 * 从虚拟地址中提取 Page Table index（中 10 位）
 */
static inline uint32_t pt_index(uint32_t virt) {
    return (virt >> 12) & 0x3FFu;
}

/*
 * memzero_page — 将一页（4KiB）清零
 *
 * [WHY] 新分配的页表必须全部清零，否则 CPU 可能把垃圾数据当作有效 PTE，
 *       访问到随机物理地址 → 数据损坏或崩溃。
 */
static void memzero_page(void* page) {
    uint32_t* p = (uint32_t*)page;
    for (uint32_t i = 0; i < VMM_PAGE_SIZE / sizeof(uint32_t); i++) {
        p[i] = 0;
    }
}

/*
 * vmm_ensure_page_table — 确保 PDE[pd_idx] 指向一个有效的页表
 *
 * 如果 PDE 的 Present 位为 0（即该页表还不存在），就从 PMM 分配一页
 * 作为新页表，清零后填入 PDE。
 *
 * [WHY] x86 的两级页表是"按需创建"的：
 *   - 页目录有 1024 个 PDE，如果全部预分配页表需要 1024×4KiB = 4MiB
 *   - 大部分虚拟地址空间是空的，不需要页表
 *   - 只在实际建立映射时才分配对应的页表，节省物理内存
 *
 * @return 页表的指针（虚拟地址，identity mapping 阶段 == 物理地址），失败返回 NULL
 */
static page_table_t* vmm_ensure_page_table(uint32_t pd_idx) {
    uint32_t pde = g_kernel_pd.entries[pd_idx];

    if (pde & PDE_PRESENT) {
        /*
         * PDE 已经有效，取出它指向的页表物理地址。
         * 高 20 位是页帧地址，低 12 位是 flags，用 & ~0xFFF 去掉 flags。
         *
         * [CRITICAL] PDE 中存储的是物理地址，不能直接当指针。
         *   必须用 PHYS_TO_VIRT 转换为虚拟地址才能解引用。
         */
        uint32_t pt_phys = pde & ~0xFFFu;
        return (page_table_t*)PHYS_TO_VIRT(pt_phys);
    }

    /*
     * PDE 不存在 → 需要分配一个新页表
     *
     * [WHY] 页表本身也要占一页物理内存（4KiB，1024 个 PTE × 4 字节）。
     *       所以我们向 PMM "批发"一页来用。
     *
     * pmm_alloc_page() 返回物理地址（void* 形式）。
     * 需要 PHYS_TO_VIRT 才能解引用（清零、填写 PTE 等）。
     * PDE 中存储物理地址（给 CPU/MMU 用）。
     */
    uint32_t pt_phys = (uint32_t)(uintptr_t)pmm_alloc_page();
    if (!pt_phys) {
        printk("[vmm] FATAL: cannot alloc page table for PD[%u]\n", (unsigned)pd_idx);
        return NULL;
    }

    /* 通过虚拟地址清零新页表（所有 PTE 的 Present=0 → 未映射） */
    page_table_t* pt_virt = (page_table_t*)PHYS_TO_VIRT(pt_phys);
    memzero_page(pt_virt);

    /*
     * 填入 PDE：高 20 位 = 页表物理地址，低位 = flags
     *
     * [BITFIELDS] PDE flags 设置说明：
     *   PDE_PRESENT  : 此 PDE 有效
     *   PDE_WRITABLE : 允许写入（具体权限还要看 PTE 的 R/W 位）
     *
     * [WHY] PDE 的 flags 是"宽松"方向的上限：
     *   - PDE 设 Writable，但 PTE 设 Read-Only → 最终该页只读
     *   - PDE 设 Read-Only，即使 PTE 设 Writable → 最终该页也只读
     *   所以我们在 PDE 层一般给足权限，精细控制放在 PTE。
     */
    g_kernel_pd.entries[pd_idx] = pt_phys | PDE_PRESENT | PDE_WRITABLE;

    return pt_virt;
}

/* ============================================================================
 * 公开 API 实现
 * ============================================================================ */

int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    /*
     * 将虚拟地址 virt 映射到物理地址 phys。
     *
     * 步骤：
     *   1. 从 virt 算出 PD index 和 PT index
     *   2. 确保该 PD index 对应的页表存在
     *   3. 在页表中写入 PTE
     *   4. 如果分页已开启，刷新 TLB
     */

    /* 对齐检查：虚拟和物理地址都必须 4KiB 对齐 */
    if ((virt & 0xFFFu) || (phys & 0xFFFu)) {
        printk("[vmm] map: unaligned virt=0x%08x phys=0x%08x\n", virt, phys);
        return -1;
    }

    uint32_t pdi = pd_index(virt);
    uint32_t pti = pt_index(virt);

    page_table_t* pt = vmm_ensure_page_table(pdi);
    if (!pt) {
        return -1;  /* PMM OOM */
    }

    /*
     * [WHY] PDE 的 U/S 位也需要匹配
     *
     * x86 两级页表的权限检查是"与"逻辑：
     *   用户态访问一个页 → 需要 PDE.U/S=1 AND PTE.U/S=1
     *   如果 PDE.U/S=0，即使 PTE.U/S=1，用户态也会触发 #PF
     *
     * vmm_ensure_page_table() 创建新 PDE 时只设了 PRESENT|WRITABLE，
     * 没有 USER。这里补上：当 flags 包含 PTE_USER 时，PDE 也加上 PDE_USER。
     * 用 |= 追加（不覆盖已有标志），不影响同一页表下的内核映射。
     */
    if (flags & PTE_USER) {
        g_kernel_pd.entries[pdi] |= PDE_USER;
    }

    /*
     * 写入 PTE：高 20 位 = phys 的页帧地址，低位 = flags
     *
     * [BITFIELDS] 常见组合：
     *   PTE_PRESENT | PTE_WRITABLE       → 内核可读写
     *   PTE_PRESENT | PTE_WRITABLE | PTE_USER → 用户态也可读写
     */
    pt->entries[pti] = (phys & ~0xFFFu) | (flags & 0xFFFu);

    /*
     * [CPU STATE] TLB 刷新
     *
     * CPU 会把最近的虚拟→物理翻译缓存在 TLB (Translation Lookaside Buffer) 中。
     * 如果我们修改了一个已缓存的映射，必须用 invlpg 指令告诉 CPU "这个地址的
     * 缓存作废了，下次访问要重新查页表"。
     *
     * 在分页还没开启时（vmm_init 过程中），TLB 还没生效，不需要刷新。
     */
    if (g_vmm_ready) {
        vmm_invlpg(virt);
    }

    return 0;
}

void vmm_unmap_page(uint32_t virt) {
    if ((virt & 0xFFFu)) {
        return;
    }

    uint32_t pdi = pd_index(virt);
    uint32_t pde = g_kernel_pd.entries[pdi];

    /* 如果 PDE 不存在，说明这个 4MiB 区域根本没有页表，无需操作 */
    if (!(pde & PDE_PRESENT)) {
        return;
    }

    uint32_t pt_phys = pde & ~0xFFFu;
    page_table_t* pt = (page_table_t*)PHYS_TO_VIRT(pt_phys);
    uint32_t pti = pt_index(virt);

    /*
     * 清除 PTE：置 0 即可（Present=0，CPU 不会再用这个条目）
     *
     * [WHY] 不释放物理页：
     *   unmap 只是"断开虚拟→物理的映射"，物理页可能还在被别处引用
     *   （如共享内存）。释放物理页是调用者的责任。
     */
    pt->entries[pti] = 0;

    if (g_vmm_ready) {
        vmm_invlpg(virt);
    }
}

void vmm_init(void) {
    /*
     * ========================================================================
     * VMM 初始化：用 4KiB 页表替换 boot.asm 的临时 PSE 映射
     * ========================================================================
     *
     * [CPU STATE] 进入此函数时：
     *   - CR0.PG = 1（boot.asm 已开启分页）
     *   - CR3 指向 boot_pd（4MiB PSE 页，临时映射）
     *   - 运行在虚拟地址 0xC01xxxxx（high-half）
     *
     * [GOAL] 建立正式的 4KiB 页表，覆盖：
     *   1. Identity mapping:  virt 0x00000000+ → phys 0x00000000+
     *      保留低地址映射，让 VGA (0xB8000)、Multiboot2 info 等仍可访问。
     *   2. High-half mapping: virt 0xC0000000+ → phys 0x00000000+
     *      内核代码/数据/栈都在这个范围。
     *
     * [WHY] 为什么保留 identity mapping？
     *   - PMM 内部的 bitmap 操作仍使用物理地址（identity mapping 下可直接访问）
     *   - Multiboot2 info 指针是物理地址
     *   - VGA text buffer 在物理 0xB8000
     *   将来移除 identity mapping 是单独的步骤。
     */

    printk("[vmm] init: replacing boot PSE with 4KiB page tables...\n");

    /* 清零页目录（所有 PDE = 0 = 不存在） */
    memzero_page(&g_kernel_pd);

    /*
     * 计算需要映射的物理范围
     *
     * [WHY] 必须覆盖 PMM 管理的所有物理页，否则 PHYS_TO_VIRT() 访问
     *   高地址页时会触发 Page Fault。PMM init 已经在 vmm_init 之前完成，
     *   所以可以查询 PMM 来确定需要映射的范围。
     */
    uint32_t kernel_end = (uint32_t)(uintptr_t)__kernel_phys_end;
    uint32_t map_end = kernel_end;

    /* 至少映射 16MiB（覆盖 VGA、BIOS 区域等） */
    uint32_t min_map = 16u * 1024u * 1024u;
    if (map_end < min_map) {
        map_end = min_map;
    }

    /* 覆盖 PMM 管理的全部物理页（buddy 可能管理到 256MiB+） */
    uint32_t pmm_end = pmm_managed_base() + pmm_total_pages() * VMM_PAGE_SIZE;
    if (pmm_end > map_end) {
        map_end = pmm_end;
    }

    /* 不超过 1GiB（内核虚拟地址空间上限 0xC0000000 + 1GiB = 0xFFFFFFFF） */
    if (map_end > 0x40000000u) {
        map_end = 0x40000000u;
    }

    /* 向上对齐到 4KiB 页边界 */
    map_end = (map_end + VMM_PAGE_SIZE - 1u) & ~(VMM_PAGE_SIZE - 1u);

    printk("[vmm] mapping phys 0x00000000 - 0x%08x\n", map_end);

    /*
     * 逐页建立双重映射：identity + high-half
     *
     * [WHY] 每个物理页 P 建立两条映射：
     *   virt P              → phys P  (identity，临时保留)
     *   virt P+0xC0000000   → phys P  (high-half，内核正式地址)
     */
    for (uint32_t addr = 0; addr < map_end; addr += VMM_PAGE_SIZE) {
        /* Identity map: virt == phys */
        int ret = vmm_map_page(addr, addr, PTE_PRESENT | PTE_WRITABLE);
        if (ret != 0) {
            printk("[vmm] FATAL: identity map failed at 0x%08x\n", addr);
            return;
        }

        /* High-half map: virt = phys + KERNEL_VIRT_OFFSET */
        uint32_t virt_high = addr + KERNEL_VIRT_OFFSET;
        ret = vmm_map_page(virt_high, addr, PTE_PRESENT | PTE_WRITABLE);
        if (ret != 0) {
            printk("[vmm] FATAL: high-half map failed at 0x%08x\n", virt_high);
            return;
        }
    }

    uint32_t pages_mapped = map_end / VMM_PAGE_SIZE;
    uint32_t pts_identity = (pages_mapped + VMM_ENTRIES_PER_PT - 1) / VMM_ENTRIES_PER_PT;
    uint32_t pts_total = pts_identity * 2;  /* identity + high-half */

    /*
     * ========================================================================
     * 切换到新页目录
     * ========================================================================
     *
     * [CPU STATE] boot.asm 的 PSE 映射 → g_kernel_pd 的 4KiB 映射
     *
     * [WHY] g_kernel_pd 在 .bss 中，虚拟地址 0xC0xxxxxx。
     *   CR3 需要物理地址，所以用 VIRT_TO_PHYS 转换。
     *
     * [CRITICAL] 新页目录必须覆盖当前 EIP 所在的虚拟地址，
     *   否则 CR3 切换后 CPU 取不到下一条指令 → #PF → Triple Fault。
     *   我们的 high-half mapping 覆盖了 0xC0000000+，EIP 在此范围内。
     */
    uint32_t pd_phys = VIRT_TO_PHYS((uint32_t)(uintptr_t)&g_kernel_pd);
    printk("[vmm] page directory: virt=0x%08x phys=0x%08x\n",
           (unsigned)(uintptr_t)&g_kernel_pd, pd_phys);
    printk("[vmm] loading new CR3...\n");

    vmm_load_page_directory(pd_phys);

    /*
     * [CPU STATE] 切换成功！
     *   - CR3 指向 g_kernel_pd（4KiB 页表）
     *   - Identity mapping:  virt [0, map_end) → phys [0, map_end)
     *   - High-half mapping: virt [0xC0000000, 0xC0000000+map_end) → phys [0, map_end)
     *   - 其他虚拟地址访问会触发 Page Fault (#PF)
     */
    g_vmm_ready = 1;

    printk("[vmm] 4KiB paging active! mapped %u KiB (%u pages, %u page tables)\n",
           (unsigned)(map_end / 1024u),
           (unsigned)(pages_mapped * 2),
           (unsigned)pts_total);
    printk("[vmm] identity:  0x00000000 - 0x%08x\n", map_end);
    printk("[vmm] high-half: 0x%08x - 0x%08x\n",
           (unsigned)KERNEL_VIRT_OFFSET,
           (unsigned)(KERNEL_VIRT_OFFSET + map_end));

    /*
     * 初始化 vmm_alloc_pages 的虚拟地址水位线
     *
     * [WHY] 直接映射区占据了 [0xC0000000, 0xC0000000 + map_end)。
     *   vmm_alloc_pages 必须从这之后开始分配，否则会和已有映射冲突。
     *   向上对齐到 4MiB 边界（0x400000），留出安全间隔，也方便调试时
     *   一眼区分"直接映射区地址"和"动态分配区地址"。
     *
     * 例如：直接映射区到 0xD0000000，水位线就从 0xD0000000 开始。
     *       直接映射区到 0xD0123000，向上对齐到 0xD0400000。
     */
    uint32_t direct_map_top = KERNEL_VIRT_OFFSET + map_end;
    uint32_t align_4m = 0x400000u;
    g_next_vaddr = (direct_map_top + align_4m - 1u) & ~(align_4m - 1u);

    g_direct_map_end = direct_map_top;

    printk("[vmm] alloc area: 0x%08x - 0xFFFFFFFF\n", g_next_vaddr);
}

void vmm_unmap_identity(void) {
    /*
     * 拆除 identity mapping（虚拟地址 0x00000000 - 0xBFFFFFFF）
     *
     * [WHY] identity mapping 只是从 boot.asm PSE 过渡到正式 4KiB 页表时的临时需要。
     *   现在内核已经完全运行在 high-half (0xC0000000+)，
     *   所有指针已经通过 PHYS_TO_VIRT 转换，可以安全拆除。
     *   拆除后，低地址空间留给将来的用户态进程。
     *
     * [CPU STATE] 拆除后：
     *   - PD[0..767] 全部清零（只清有效条目）
     *   - 访问 0x00000000 - 0xBFFFFFFF 将触发 #PF
     *   - 重新加载 CR3 刷新整个 TLB
     *
     * [NOTE] 被清除的 PDE 所指向的页表物理页未释放（微量泄漏，将来进程管理进入后可回收）。
     */

    if (!g_vmm_ready) {
        return;
    }

    uint32_t first_kernel_pde = pd_index(KERNEL_VIRT_OFFSET); /* 768 */
    uint32_t cleared = 0;

    for (uint32_t i = 0; i < first_kernel_pde; i++) {
        if (g_kernel_pd.entries[i] & PDE_PRESENT) {
            g_kernel_pd.entries[i] = 0;
            cleared++;
        }
    }

    /* 重新加载 CR3 做全局 TLB 刷新（比逐页 invlpg 更彻底） */
    uint32_t pd_phys = VIRT_TO_PHYS((uint32_t)(uintptr_t)&g_kernel_pd);
    vmm_load_page_directory(pd_phys);

    printk("[vmm] identity mapping removed (%u PDEs cleared)\n", (unsigned)cleared);
}

/* ============================================================================
 * 查询辅助函数（供 shell vmm 命令使用）
 * ============================================================================ */

int vmm_is_ready(void) {
    return g_vmm_ready;
}

uint32_t vmm_direct_map_end(void) {
    return g_direct_map_end;
}

int vmm_is_mapped(uint32_t virt) {
    if (!g_vmm_ready) {
        return -1;
    }
    if (virt & 0xFFFu) {
        return -1;
    }

    uint32_t pdi = pd_index(virt);
    uint32_t pde = g_kernel_pd.entries[pdi];
    if (!(pde & PDE_PRESENT)) {
        return 0;
    }

    uint32_t pt_phys = pde & ~0xFFFu;
    page_table_t* pt = (page_table_t*)PHYS_TO_VIRT(pt_phys);
    uint32_t pti = pt_index(virt);

    return (pt->entries[pti] & PTE_PRESENT) ? 1 : 0;
}

int vmm_get_physical(uint32_t virt, uint32_t* phys) {
    if (!g_vmm_ready || !phys) {
        return -1;
    }
    if (virt & 0xFFFu) {
        return -1;
    }

    uint32_t pdi = pd_index(virt);
    uint32_t pde = g_kernel_pd.entries[pdi];
    if (!(pde & PDE_PRESENT)) {
        return -1;
    }

    uint32_t pt_phys = pde & ~0xFFFu;
    page_table_t* pt = (page_table_t*)PHYS_TO_VIRT(pt_phys);
    uint32_t pti = pt_index(virt);

    uint32_t pte = pt->entries[pti];
    if (!(pte & PTE_PRESENT)) {
        return -1;
    }

    *phys = pte & ~0xFFFu;
    return 0;
}

uint32_t vmm_get_pde(uint32_t pd_idx) {
    if (!g_vmm_ready || pd_idx >= VMM_ENTRIES_PER_PD) {
        return 0;
    }
    return g_kernel_pd.entries[pd_idx];
}

uint32_t vmm_get_pte(uint32_t virt) {
    if (!g_vmm_ready) {
        return 0;
    }

    uint32_t pdi = pd_index(virt);
    uint32_t pde = g_kernel_pd.entries[pdi];
    if (!(pde & PDE_PRESENT)) {
        return 0;
    }

    uint32_t pt_phys = pde & ~0xFFFu;
    page_table_t* pt = (page_table_t*)PHYS_TO_VIRT(pt_phys);
    uint32_t pti = pt_index(virt);

    return pt->entries[pti];
}

/* ============================================================================
 * vmm_alloc_pages / vmm_free_pages — 虚拟页批量分配与释放
 *
 * [WHY] kmalloc 等高层分配器需要连续的虚拟内存区域。
 *   PMM 给的物理页是散的，这两个函数负责：
 *   - 挑一段空闲的连续虚拟地址（bump allocator, g_next_vaddr）
 *   - 向 PMM 申请物理页
 *   - 用 vmm_map_page 建立映射
 *   返回的指针可以直接读写，MMU 自动翻译。
 * ============================================================================ */

void* vmm_alloc_pages(unsigned count) {
    /* 步骤 1: 前置检查 */
    if (count == 0 || !g_vmm_ready || g_next_vaddr == 0) {
        return NULL;
    }

    /* 步骤 2: 计算地址范围，检查溢出 */
    uint32_t start = g_next_vaddr;
    uint32_t total_bytes = count * VMM_PAGE_SIZE;
    if (total_bytes > 0xFFFFFFFFu - start) {
        return NULL;
    }

    /* 步骤 3: 推进水位线 */
    g_next_vaddr = start + total_bytes;

    /* 步骤 4: 逐页分配物理页并建立映射 */
    for (unsigned i = 0; i < count; i++) {
        uint32_t virt = start + i * VMM_PAGE_SIZE;
        uint32_t phys = (uint32_t)(uintptr_t)pmm_alloc_page();

        if (!phys || vmm_map_page(virt, phys, PTE_PRESENT | PTE_WRITABLE) != 0) {
            /*
             * OOM 或 map 失败 → 释放当前页（如果已分配）+ 回滚前面的页
             *
             * [WHY] 用 goto 做错误回滚是 Linux 内核标准风格。
             *   把清理逻辑集中在一处，避免重复代码。
             */
            if (phys) {
                pmm_free_page((void*)(uintptr_t)phys);
            }
            goto rollback;
        }
    }

    /* 步骤 5: 注册 VMA（如果 VMA 子系统已就绪） */
    if (vma_is_ready()) {
        vma_add(start, start + total_bytes, VMA_READ | VMA_WRITE, "vmm-alloc");
    }

    /* 步骤 6: 成功，返回起始虚拟地址 */
    return (void*)(uintptr_t)start;

rollback:
    /*
     * 回滚已映射的页：先查物理地址 → 释放物理页 → 解除映射
     *
     * [WHY] 顺序很重要：vmm_unmap_page 会清除 PTE，之后就查不到物理地址了。
     *   所以必须在 unmap 之前用 vmm_get_physical 拿到物理地址。
     */
    for (unsigned j = 0; j < count; j++) {
        uint32_t rollback_virt = start + j * VMM_PAGE_SIZE;
        uint32_t rollback_phys;
        if (vmm_get_physical(rollback_virt, &rollback_phys) == 0) {
            pmm_free_page((void*)(uintptr_t)rollback_phys);
            vmm_unmap_page(rollback_virt);
        }
    }
    return NULL;
}

void vmm_free_pages(void* vaddr, unsigned count) {
    /*
     * 释放由 vmm_alloc_pages 分配的虚拟页
     *
     * 逐页执行：查页表拿物理地址 → 归还物理页 → 清除映射
     *
     * [WHY] 顺序必须是 get_physical → free → unmap。
     *   unmap 会清零 PTE，之后就查不到物理地址了。
     *
     * [NOTE] 虚拟地址空间不回收（bump allocator 的代价）。
     *   将来 Stage 9 (VMA) 会用红黑树跟踪空闲虚拟区域。
     */
    if (!vaddr || count == 0 || !g_vmm_ready) {
        return;
    }

    uint32_t base = (uint32_t)(uintptr_t)vaddr;

    /* 移除 VMA 记录（如果 VMA 子系统已就绪） */
    if (vma_is_ready()) {
        vma_remove(base);
    }

    for (unsigned i = 0; i < count; i++) {
        uint32_t virt = base + i * VMM_PAGE_SIZE;
        uint32_t phys;

        if (vmm_get_physical(virt, &phys) == 0) {
            pmm_free_page((void*)(uintptr_t)phys);
        }
        vmm_unmap_page(virt);
    }
}
