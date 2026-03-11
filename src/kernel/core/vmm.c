#include <stdint.h>
#include <stddef.h>

#include "vmm.h"
#include "pmm.h"
#include "printk.h"

/*
 * Virtual Memory Manager (VMM) — x86 32-bit 两级页表
 *
 * ============================================================================
 * [WHY] 为什么要开启分页？
 * ============================================================================
 *
 * 目前内核运行在"实地址 == 线性地址"的模式（identity mapping，靠 GDT 平坦段）。
 * 虽然能用，但有几个问题：
 *   1. 无法给用户态进程独立的地址空间（Ring 3 隔离）
 *   2. 无法实现按需分配（page fault handler）
 *   3. 无法把内核映射到高地址（High-Half Kernel）
 *
 * 开启分页后，每次内存访问都经过 MMU 翻译：
 *   虚拟地址 → (Page Directory → Page Table) → 物理地址
 *
 * ============================================================================
 * [CPU STATE] 开启分页的关键操作
 * ============================================================================
 *
 * 1. 把 Page Directory 的 **物理地址** 写入 CR3
 *    → CPU 知道去哪里找顶层页表
 *
 * 2. 设置 CR0 的 PG 位 (bit 31)
 *    → 从下一条指令开始，所有地址都经过分页翻译
 *
 * [CRITICAL] 开启分页的那一瞬间，EIP（指令指针）里的地址也会被翻译。
 *   如果我们没有把当前代码所在的物理地址做 identity mapping，
 *   CPU 翻译 EIP 会得到一个无效地址 → Page Fault → Triple Fault → 重启。
 *   所以我们 **必须** 先把内核用到的所有物理地址 1:1 映射好。
 */

/* Linker 导出的内核物理边界（定义在 linker.ld） */
extern char __kernel_phys_start[];
extern char __kernel_phys_end[];

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
         * [ASSUMPTION] identity mapping 阶段，物理地址可以直接当指针用。
         */
        uint32_t pt_phys = pde & ~0xFFFu;
        return (page_table_t*)(uintptr_t)pt_phys;
    }

    /*
     * PDE 不存在 → 需要分配一个新页表
     *
     * [WHY] 页表本身也要占一页物理内存（4KiB，1024 个 PTE × 4 字节）。
     *       所以我们向 PMM "批发"一页来用。
     */
    void* new_pt = pmm_alloc_page();
    if (!new_pt) {
        printk("[vmm] FATAL: cannot alloc page table for PD[%u]\n", (unsigned)pd_idx);
        return NULL;
    }

    /* 新页表必须全部清零（所有 PTE 的 Present=0 → 未映射） */
    memzero_page(new_pt);

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
    uint32_t pt_phys = (uint32_t)(uintptr_t)new_pt;
    g_kernel_pd.entries[pd_idx] = pt_phys | PDE_PRESENT | PDE_WRITABLE;

    return (page_table_t*)new_pt;
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
    page_table_t* pt = (page_table_t*)(uintptr_t)pt_phys;
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
     * VMM 初始化：建立 identity mapping 并开启分页
     * ========================================================================
     *
     * [GOAL] 开启分页后，内核代码还能正常运行。
     *
     * [STRATEGY] Identity Mapping（恒等映射）：
     *   对于内核当前使用的每一个物理地址 P，都映射 virt=P → phys=P。
     *   这样开启分页后，之前所有用到的地址（代码、数据、栈、PMM bitmap、
     *   VGA buffer...）都仍然有效。
     *
     * [WHY] 需要映射哪些范围？
     *
     *   1. 低 1MiB (0x00000 - 0xFFFFF):
     *      包含 VGA text buffer (0xB8000)、BIOS 数据区等。
     *      虽然内核代码不在这里，但 VGA 输出/BIOS 中断需要它。
     *
     *   2. 内核镜像 (__kernel_phys_start - __kernel_phys_end):
     *      内核代码、只读数据、全局变量、BSS（包括这个页目录自己！）
     *
     *   3. PMM 管理的内存（已被分配的页）:
     *      PMM 的 bitmap 存储在 usable RAM 区域的开头，必须可读写。
     *
     *   简单做法：把 0 到 "内核结束地址或 PMM 管理范围结束" 都 identity map。
     *   由于我们在 QEMU 里通常只有几十 MiB，多映射一些也不会浪费太多页表。
     */

    printk("[vmm] init: setting up identity mapping...\n");

    /* 清零页目录（所有 PDE = 0 = 不存在） */
    memzero_page(&g_kernel_pd);

    /*
     * 计算需要 identity map 的范围
     *
     * 我们取一个保守的上界：MAX(kernel_end, 16MiB)
     * - 16MiB 足以覆盖低 1MiB + 内核 + PMM bitmap（小内存场景）
     * - 如果内核镜像超过 16MiB（不太可能），也能覆盖
     *
     * [WHY] 为什么不精确计算？
     *   精确算需要遍历 PMM 所有 region，代码复杂且收益不大。
     *   多映射几 MiB 只多用几个页表（每个页表映射 4MiB），overhead 很小。
     */
    uint32_t kernel_end = (uint32_t)(uintptr_t)__kernel_phys_end;
    uint32_t map_end = kernel_end;

    /* 至少映射到 16MiB，保证覆盖低端内存和常见的 PMM 区域 */
    uint32_t min_map = 16u * 1024u * 1024u;   /* 16 MiB */
    if (map_end < min_map) {
        map_end = min_map;
    }

    /* 向上对齐到 4KiB 页边界 */
    map_end = (map_end + VMM_PAGE_SIZE - 1u) & ~(VMM_PAGE_SIZE - 1u);

    printk("[vmm] identity map: 0x00000000 - 0x%08x\n", map_end);

    /*
     * 逐页建立 identity mapping: virt == phys
     *
     * [WHY] 为什么一页一页映射，而不是用 4MiB 大页？
     *   4MiB 大页 (PSE) 更快，但需要设置 CR4.PSE 位，且粒度太粗
     *   （无法对单个 4KiB 页设置不同权限）。用 4KiB 页更灵活，
     *   适合学习阶段理解两级页表的完整流程。
     */
    for (uint32_t addr = 0; addr < map_end; addr += VMM_PAGE_SIZE) {
        int ret = vmm_map_page(addr, addr, PTE_PRESENT | PTE_WRITABLE);
        if (ret != 0) {
            printk("[vmm] FATAL: identity map failed at 0x%08x\n", addr);
            return;
        }
    }

    uint32_t pages_mapped = map_end / VMM_PAGE_SIZE;
    uint32_t pts_used = (pages_mapped + VMM_ENTRIES_PER_PT - 1) / VMM_ENTRIES_PER_PT;
    printk("[vmm] mapped %u pages (%u page tables)\n",
           (unsigned)pages_mapped, (unsigned)pts_used);

    /*
     * ========================================================================
     * 关键时刻：加载 CR3 并开启分页
     * ========================================================================
     *
     * [CPU STATE] 执行顺序：
     *
     * 1. vmm_load_page_directory(pd_phys)
     *    → mov cr3, eax
     *    → CR3 现在指向我们的页目录
     *    → 此时 CR0.PG 仍为 0，分页还没开
     *
     * 2. vmm_enable_paging()
     *    → mov eax, cr0 ; or eax, 0x80000000 ; mov cr0, eax
     *    → CR0.PG (bit 31) 置 1
     *    → 从 **下一条指令** 开始，所有内存访问都经过分页翻译
     *    → 如果 identity mapping 正确，EIP 指向的物理地址 == 虚拟地址，
     *      CPU 能正常取到下一条指令，一切继续运行
     *    → 如果 identity mapping 有 bug，CPU 取不到下一条指令 → #PF → Triple Fault
     *
     * [WHY] 为什么分成两步（先 CR3 再 CR0.PG）？
     *   Intel 手册规定：必须先把有效的页目录地址放入 CR3，再开 PG。
     *   如果先开 PG 而 CR3 指向垃圾 → 立即 Triple Fault。
     */
    uint32_t pd_phys = (uint32_t)(uintptr_t)&g_kernel_pd;
    printk("[vmm] page directory @ 0x%08x\n", pd_phys);
    printk("[vmm] loading CR3 and enabling paging...\n");

    vmm_load_page_directory(pd_phys);
    vmm_enable_paging();

    /*
     * 如果执行到这里，说明分页成功开启了！
     *
     * [CPU STATE] 现在的状态：
     *   - CR0.PG = 1 (分页已开启)
     *   - CR3 = pd_phys (指向内核页目录)
     *   - 所有虚拟地址 virt ∈ [0, map_end) 都映射到 phys == virt
     *   - 其他虚拟地址访问会触发 Page Fault (#PF, int 14)
     */
    g_vmm_ready = 1;

    printk("[vmm] paging enabled!\n");
    printk("[vmm] identity map: %u KiB (%u pages, %u page tables)\n",
           (unsigned)(map_end / 1024u),
           (unsigned)pages_mapped,
           (unsigned)pts_used);
}

/* ============================================================================
 * 查询辅助函数（供 shell vmm 命令使用）
 * ============================================================================ */

int vmm_is_ready(void) {
    return g_vmm_ready;
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
    page_table_t* pt = (page_table_t*)(uintptr_t)pt_phys;
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
    page_table_t* pt = (page_table_t*)(uintptr_t)pt_phys;
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
    page_table_t* pt = (page_table_t*)(uintptr_t)pt_phys;
    uint32_t pti = pt_index(virt);

    return pt->entries[pti];
}
