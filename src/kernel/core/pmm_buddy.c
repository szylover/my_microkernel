#include <stdint.h>
#include <stddef.h>

#include "pmm.h"
#include "vmm.h"
#include "printk.h"

/*
 * Physical Memory Manager (PMM) — Buddy System 后端
 *
 * ============================================================================
 * [WHY] Buddy System 概览
 * ============================================================================
 *
 * Linux 内核用 buddy allocator 管理物理页：
 *   - 每个"空闲块"大小为 2^order 页（order 0..MAX_ORDER）
 *   - 每个 order 维护一个空闲双向链表
 *   - 分配时：从目标 order 的链表取；若空则从更高 order 分裂
 *   - 释放时：检查伙伴是否也空闲，若是则合并为更大块，逐级向上
 *
 * 伙伴计算：
 *   buddy_index = page_index ^ (1 << order)
 *   两个伙伴合并后的 parent_index = page_index & ~(1 << order)
 *
 * 本文件是 pmm_ops_t 的第二个后端实现。
 * 通过 pmm_buddy_get_ops() 导出操作表，由 pmm.c dispatch 层调用。
 *
 * [CPU STATE] PMM 纯软件数据结构，不修改 CR0/CR3/特权级。
 */

/* ============================================================================
 * Multiboot2 解析（与 bitmap 后端共用相同结构体定义）
 * ============================================================================ */

extern const void* g_mb2_info;
extern char __kernel_phys_start[];
extern char __kernel_phys_end[];

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} __attribute__((packed));

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

/* ============================================================================
 * 常量
 * ============================================================================ */

/*
 * BUDDY_MAX_ORDER — 最大阶数
 *
 * [WHY] order=10 → 最大块 2^10 = 1024 页 = 4MiB。
 *   与 Linux 默认 MAX_ORDER=11（即 order 0..10）一致。
 */
#define BUDDY_MAX_ORDER  10
#define PAGE_SIZE        4096u

/* ============================================================================
 * 小工具
 * ============================================================================ */

static uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

static uint32_t align_down(uint32_t v, uint32_t a) {
    return v & ~(a - 1u);
}

static void memzero(void* dst, uint32_t bytes) {
    uint8_t* p = (uint8_t*)dst;
    for (uint32_t i = 0; i < bytes; i++) p[i] = 0;
}

/* ============================================================================
 * Buddy 核心数据结构
 * ============================================================================ */

/*
 * free_node_t — 侵入式双向链表节点
 *
 * [WHY] 与 Linux 的 struct list_head 用法一致：
 *   - 插入/删除 O(1)
 *   - 每个空闲块用所在页的 page_info 里的 node 字段链入 free_list
 */
typedef struct free_node {
    struct free_node* prev;
    struct free_node* next;
} free_node_t;

/*
 * page_info_t — 每个物理页的元数据
 *
 * [WHY] buddy 合并时需要：
 *   - order：记录当前块大小（仅块首页有效）
 *   - is_free：快速判断伙伴是否空闲
 *   - node：链入 free_list
 *
 * Linux 的 struct page 远比这复杂（flags、refcount、mapping 等），
 * 我们只保留 buddy 所需最小字段。
 *
 * 内存开销：每页 16 字节。256MiB RAM → 65536 页 → 1MiB 元数据。
 */
typedef struct {
    free_node_t node;       /* 空闲链表节点（仅空闲块首页使用） */
    uint8_t     order;      /* 当前块的 order（仅块首页有效） */
    uint8_t     is_free;    /* 1=空闲, 0=已分配 */
    uint8_t     _pad[2];
} page_info_t;

/*
 * buddy_zone_t — buddy 管理区域
 *
 * [WHY] 单 zone 设计（vs Linux 多 zone）：
 *   学习级内核不需要 DMA zone / HighMem 区分。
 *   管理一个最大的连续物理区域。
 */
typedef struct {
    uint32_t     base_phys;     /* 管理区域物理起始地址（页对齐） */
    uint32_t     total_pages;   /* 管理区域总页数 */
    page_info_t* pages;         /* page_info 数组（虚拟地址） */
    free_node_t  free_list[BUDDY_MAX_ORDER + 1]; /* 头节点（哨兵） */
    uint32_t     free_count[BUDDY_MAX_ORDER + 1]; /* 每个 order 的空闲块数 */
    uint32_t     total_free;    /* 空闲页总数 */
} buddy_zone_t;

static buddy_zone_t g_zone;
static int g_buddy_ready = 0;

/* ============================================================================
 * 双向链表操作
 * ============================================================================ */

static void list_init(free_node_t* head) {
    head->prev = head;
    head->next = head;
}

static int list_empty(const free_node_t* head) {
    return head->next == head;
}

static void list_add_tail(free_node_t* node, free_node_t* head) {
    node->prev = head->prev;
    node->next = head;
    head->prev->next = node;
    head->prev = node;
}

static void list_del(free_node_t* node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node;
    node->next = node;
}

static free_node_t* list_pop(free_node_t* head) {
    if (list_empty(head)) return NULL;
    free_node_t* node = head->next;
    list_del(node);
    return node;
}

/* ============================================================================
 * Buddy 辅助函数
 * ============================================================================ */

static uint32_t page_index(const page_info_t* p) {
    return (uint32_t)(p - g_zone.pages);
}

static page_info_t* index_to_page(uint32_t idx) {
    return &g_zone.pages[idx];
}

static uint32_t index_to_phys(uint32_t idx) {
    return g_zone.base_phys + idx * PAGE_SIZE;
}

static uint32_t phys_to_index(uint32_t phys) {
    return (phys - g_zone.base_phys) / PAGE_SIZE;
}

/*
 * buddy_index — 计算伙伴的页索引
 *
 * [WHY] 伙伴关系：在 order=k 下，块大小为 2^k 页。
 *   两个相邻块组成一对伙伴，index 通过 XOR (1<<k) 得到。
 */
static uint32_t calc_buddy_index(uint32_t idx, unsigned order) {
    return idx ^ (1u << order);
}

/*
 * buddy_add_free — 将块标记为空闲并加入 free_list
 */
static void buddy_add_free(uint32_t idx, unsigned order) {
    page_info_t* p = index_to_page(idx);
    p->order = (uint8_t)order;
    p->is_free = 1;
    /*
     * [WHY] 尾插法：保持 free_list 内低地址在前。
     *   分配时从 head 取 → 优先分配低地址页 → 与 boot PSE 映射兼容。
     *   （boot.asm 只映射前 16MiB，pmm_init selftest 在 vmm_init 之前运行）
     */
    list_add_tail(&p->node, &g_zone.free_list[order]);
    g_zone.free_count[order]++;
    g_zone.total_free += (1u << order);
}

/*
 * buddy_remove_free — 从 free_list 移除一个空闲块
 */
static void buddy_remove_free(uint32_t idx, unsigned order) {
    page_info_t* p = index_to_page(idx);
    list_del(&p->node);
    p->is_free = 0;
    g_zone.free_count[order]--;
    g_zone.total_free -= (1u << order);
}

/* ============================================================================
 * Buddy 核心算法
 * ============================================================================ */

/*
 * buddy_alloc — 分配 2^order 页
 *
 * [WHY] 分配流程（Linux __rmqueue 简化版）：
 *   1. 从 free_list[order] 开始向上找有空闲块的 level
 *   2. 找到后逐级分裂：高 order 块拆成两个低 order 伙伴，
 *      一个放回 free_list，另一个继续分裂或返回
 *
 * 例：order=0, 找到 order=3 的 8 页块：
 *   [0..7] → [0..3]+[4..7]，[4..7]→free_list[2]
 *   [0..3] → [0..1]+[2..3]，[2..3]→free_list[1]
 *   [0..1] → [0]+[1]，[1]→free_list[0]
 *   返回 [0]
 */
static uint32_t buddy_alloc(unsigned order) {
    unsigned cur;
    for (cur = order; cur <= BUDDY_MAX_ORDER; cur++) {
        if (!list_empty(&g_zone.free_list[cur]))
            break;
    }
    if (cur > BUDDY_MAX_ORDER) return UINT32_MAX;

    /* 取出块 */
    free_node_t* node = list_pop(&g_zone.free_list[cur]);
    /*
     * 从 free_node_t 指针反算 page_info_t 指针
     * [WHY] node 是 page_info_t 的第一个字段，所以地址相同。
     */
    page_info_t* pg = (page_info_t*)node;
    uint32_t idx = page_index(pg);
    pg->is_free = 0;
    g_zone.free_count[cur]--;
    g_zone.total_free -= (1u << cur);

    /* 逐级分裂 */
    while (cur > order) {
        cur--;
        uint32_t buddy_idx = idx + (1u << cur);
        buddy_add_free(buddy_idx, cur);
    }

    pg->order = (uint8_t)order;
    return idx;
}

/*
 * buddy_free — 释放 2^order 页并尝试合并伙伴
 *
 * [WHY] 释放流程（Linux __free_one_page 简化版）：
 *   1. 将块标记为空闲
 *   2. 查看伙伴：若空闲且同 order → 从 free_list 移除伙伴，两者合并
 *   3. 重复直到伙伴不空闲或到达 MAX_ORDER
 */
static void buddy_free(uint32_t idx, unsigned order) {
    while (order < BUDDY_MAX_ORDER) {
        uint32_t bi = calc_buddy_index(idx, order);
        if (bi >= g_zone.total_pages) break;

        page_info_t* buddy = index_to_page(bi);
        if (!buddy->is_free || buddy->order != (uint8_t)order)
            break;

        buddy_remove_free(bi, order);
        idx = idx & ~(1u << order);  /* parent = 较小的 index */
        order++;
    }
    buddy_add_free(idx, order);
}

/* ============================================================================
 * pmm_ops_t 接口实现（放在 init 之前，供 selftest 调用）
 * ============================================================================ */

void* pmm_buddy_alloc_page(void) {
    if (!g_buddy_ready) return NULL;
    if (g_zone.total_free == 0) return NULL;

    uint32_t idx = buddy_alloc(0);
    if (idx == UINT32_MAX) return NULL;
    return (void*)(uintptr_t)index_to_phys(idx);
}

void pmm_buddy_free_page(void* page) {
    if (!g_buddy_ready || !page) return;

    uint32_t phys = (uint32_t)(uintptr_t)page;
    if ((phys & (PAGE_SIZE - 1u)) != 0) {
        printk("[pmm-buddy] free: unaligned 0x%08x\n", phys);
        return;
    }
    if (phys < g_zone.base_phys) {
        printk("[pmm-buddy] free: below managed base 0x%08x\n", phys);
        return;
    }
    uint32_t idx = phys_to_index(phys);
    if (idx >= g_zone.total_pages) {
        printk("[pmm-buddy] free: out of range 0x%08x\n", phys);
        return;
    }
    page_info_t* pg = index_to_page(idx);
    if (pg->is_free) {
        printk("[pmm-buddy] free: double free 0x%08x\n", phys);
        return;
    }
    buddy_free(idx, pg->order);
}

/* ============================================================================
 * Multiboot2 mmap 查找
 * ============================================================================ */

static const struct mb2_tag* mb2_find_mmap(const uint8_t* base, uint32_t total) {
    if (!base || total < 16) return NULL;
    const uint8_t* p = base + 8;
    const uint8_t* end = base + total;
    while (p + 8 <= end) {
        const struct mb2_tag* tag = (const struct mb2_tag*)p;
        if (tag->type == 0 && tag->size == 8) return NULL;
        if (tag->size < 8 || p + tag->size > end) return NULL;
        if (tag->type == 6) return tag;
        p += (tag->size + 7u) & ~7u;
    }
    return NULL;
}

/* ============================================================================
 * PMM Buddy 初始化
 * ============================================================================ */

static void pmm_buddy_init(void) {
    g_buddy_ready = 0;
    memzero(&g_zone, sizeof(g_zone));
    for (unsigned i = 0; i <= BUDDY_MAX_ORDER; i++)
        list_init(&g_zone.free_list[i]);

    if (!g_mb2_info) {
        printk("[pmm-buddy] no multiboot2 info; disabled\n");
        return;
    }

    const uint8_t* mb2_base = (const uint8_t*)PHYS_TO_VIRT((uint32_t)(uintptr_t)g_mb2_info);
    uint32_t mb2_total = *(const uint32_t*)(mb2_base + 0);

    const struct mb2_tag* tag = mb2_find_mmap(mb2_base, mb2_total);
    if (!tag) {
        printk("[pmm-buddy] mmap tag not found; disabled\n");
        return;
    }

    const struct mb2_tag_mmap* mmap = (const struct mb2_tag_mmap*)tag;
    if (mmap->size < sizeof(struct mb2_tag_mmap) ||
        mmap->entry_size < sizeof(struct mb2_mmap_entry)) {
        printk("[pmm-buddy] invalid mmap tag; disabled\n");
        return;
    }

    /*
     * Step 1: 找最大的 usable region（>= 1MiB, 32-bit 以内）
     *
     * [WHY] 低于 1MiB 有 BIOS/VGA 等传统设备，不纳入管理。
     */
    const uint8_t* entries = (const uint8_t*)mmap + sizeof(struct mb2_tag_mmap);
    const uint8_t* entries_end = (const uint8_t*)mmap + mmap->size;

    uint32_t best_start = 0, best_end = 0;
    for (const uint8_t* p = entries; p + mmap->entry_size <= entries_end;
         p += mmap->entry_size) {
        const struct mb2_mmap_entry* e = (const struct mb2_mmap_entry*)p;
        if (e->type != 1u) continue;

        uint64_t s64 = e->addr, e64 = e->addr + e->len;
        if (s64 >= 0x100000000ull) continue;
        uint32_t rs = (uint32_t)s64;
        uint32_t re = (e64 >= 0x100000000ull) ? 0xFFFFFFFFu : (uint32_t)e64;
        if (re <= 0x100000u) continue;
        if (rs < 0x100000u) rs = 0x100000u;
        if (re <= rs) continue;
        if ((re - rs) > (best_end - best_start)) {
            best_start = rs;
            best_end = re;
        }
    }

    if (best_end <= best_start || (best_end - best_start) < 32u * PAGE_SIZE) {
        printk("[pmm-buddy] no usable region large enough; disabled\n");
        return;
    }

    printk("[pmm-buddy] best region: 0x%08x - 0x%08x (%u KiB)\n",
           best_start, best_end, (best_end - best_start) / 1024u);

    /*
     * Step 2: 跳过保留区域（内核镜像 + Multiboot2 info），然后布局
     *
     * [WHY] best region 通常从 1MiB 开始，但内核也加载在 1MiB。
     *   page_info 数组放在区域开头 → 不能覆盖内核镜像或 mb2 info。
     *   解决方法：将 region_start 推到内核和 mb2 info 之后。
     */
    uint32_t k_end_phys  = align_up((uint32_t)(uintptr_t)__kernel_phys_end, PAGE_SIZE);
    uint32_t mb2_phys    = (uint32_t)(uintptr_t)g_mb2_info;
    const uint8_t* mb2v  = (const uint8_t*)PHYS_TO_VIRT(mb2_phys);
    uint32_t mb2_size    = *(const uint32_t*)(mb2v + 0);
    uint32_t mb2_end_p   = align_up(mb2_phys + mb2_size, PAGE_SIZE);

    /* region_start 必须在内核和 mb2 之后 */
    uint32_t safe_start = best_start;
    if (k_end_phys > safe_start && k_end_phys <= best_end)
        safe_start = k_end_phys;
    if (mb2_end_p > safe_start && mb2_end_p <= best_end)
        safe_start = mb2_end_p;

    uint32_t region_start = align_up(safe_start, PAGE_SIZE);
    uint32_t region_end   = align_down(best_end, PAGE_SIZE);
    uint32_t region_pages = (region_end - region_start) / PAGE_SIZE;

    uint32_t info_bytes = region_pages * (uint32_t)sizeof(page_info_t);
    uint32_t info_pages = align_up(info_bytes, PAGE_SIZE) / PAGE_SIZE;

    if (info_pages >= region_pages) {
        printk("[pmm-buddy] region too small for metadata; disabled\n");
        return;
    }

    uint32_t metadata_end   = region_start + info_pages * PAGE_SIZE;
    uint32_t managed_start  = metadata_end;
    uint32_t managed_end    = region_end;
    uint32_t managed_pages  = (managed_end - managed_start) / PAGE_SIZE;

    if (managed_pages < 16) {
        printk("[pmm-buddy] too few pages (%u); disabled\n", managed_pages);
        return;
    }

    /*
     * Step 3: 初始化 page_info 数组 + free_list
     */
    g_zone.base_phys   = managed_start;
    g_zone.total_pages = managed_pages;
    g_zone.pages       = (page_info_t*)PHYS_TO_VIRT(region_start);
    g_zone.total_free  = 0;

    memzero(g_zone.pages, managed_pages * (uint32_t)sizeof(page_info_t));

    /*
     * Step 4: 将所有页以最大可能 order 放入 free_list
     *
     * [WHY] 从低地址到高地址遍历，每次取最大对齐块。
     *   比逐页加入后合并更高效。
     *
     * 例 managed_pages=13:
     *   idx 0:  order 3 (8页) — idx 对齐到 8
     *   idx 8:  order 2 (4页)
     *   idx 12: order 0 (1页)
     */
    uint32_t idx = 0;
    while (idx < managed_pages) {
        unsigned order = 0;
        for (unsigned o = BUDDY_MAX_ORDER; o > 0; o--) {
            uint32_t block = 1u << o;
            if ((idx & (block - 1u)) == 0 && idx + block <= managed_pages) {
                order = o;
                break;
            }
        }
        buddy_add_free(idx, order);
        idx += (1u << order);
    }

    g_buddy_ready = 1;

    /*
     * Step 5: 验证保留区域
     *
     * [WHY] Step 2 已经把 region_start 推到内核和 mb2 info 之后，
     *   所以它们都在 managed_start 之前，不在 buddy 管理范围内。
     *   这里做一次断言性检查。
     */
    if (k_end_phys > managed_start) {
        printk("[pmm-buddy] WARNING: kernel overlaps managed zone!\n");
    }
    if (mb2_end_p > managed_start) {
        printk("[pmm-buddy] WARNING: mb2 info overlaps managed zone!\n");
    }

    /* Summary */
    printk("[pmm-buddy] zone: 0x%08x - 0x%08x (%u pages, %u KiB)\n",
           managed_start, managed_end, managed_pages, managed_pages * 4u);
    printk("[pmm-buddy] metadata: 0x%08x - 0x%08x (%u pages, %u B/page)\n",
           region_start, metadata_end, info_pages, (unsigned)sizeof(page_info_t));
    printk("[pmm-buddy] free: %u pages (%u KiB)\n",
           g_zone.total_free, g_zone.total_free * 4u);

    for (unsigned o = BUDDY_MAX_ORDER; ; o--) {
        if (g_zone.free_count[o] > 0) {
            printk("[pmm-buddy]   order %2u (%4u KiB): %u blocks\n",
                   o, (1u << o) * 4u, g_zone.free_count[o]);
        }
        if (o == 0) break;
    }

    /* Selftest
     *
     * [WHY] 此时 vmm_init 还没运行，boot PSE 只映射了前 16MiB。
     *   buddy 可能分配到高地址页（超出 16MiB），不能做 PHYS_TO_VIRT 写测试。
     *   只验证 alloc/free 计数正确性。内存读写测试在 vmm_init 之后由 shell 验证。
     */
    printk("[pmm-buddy] selftest: ");
    uint32_t free_before = g_zone.total_free;
    void* p1 = pmm_buddy_alloc_page();
    void* p2 = pmm_buddy_alloc_page();
    if (!p1 || !p2 || p1 == p2) {
        printk("FAIL (alloc)\n");
        if (p1) pmm_buddy_free_page(p1);
        if (p2) pmm_buddy_free_page(p2);
        return;
    }

    int ok = (g_zone.total_free == free_before - 2);
    pmm_buddy_free_page(p1);
    pmm_buddy_free_page(p2);
    ok = ok && (g_zone.total_free == free_before);

    printk("%s (alloc p1=%p p2=%p)\n", ok ? "OK" : "FAIL", p1, p2);
}

/* ============================================================================
 * 剩余 ops 接口 + 操作表
 * ============================================================================ */

static unsigned pmm_buddy_total_pages(void) {
    return g_buddy_ready ? (unsigned)g_zone.total_pages : 0u;
}

static unsigned pmm_buddy_free_pages(void) {
    return g_buddy_ready ? (unsigned)g_zone.total_free : 0u;
}

static unsigned pmm_buddy_managed_base(void) {
    return g_buddy_ready ? (unsigned)g_zone.base_phys : 0u;
}

static unsigned pmm_buddy_page_addr(unsigned page_index) {
    if (!g_buddy_ready || page_index >= g_zone.total_pages) return 0u;
    return (unsigned)index_to_phys(page_index);
}

static int pmm_buddy_page_is_used(unsigned page_index) {
    if (!g_buddy_ready) return -1;
    if (page_index >= g_zone.total_pages) return -1;
    return g_zone.pages[page_index].is_free ? 0 : 1;
}

/* ============================================================================
 * pmm_ops_t — buddy 后端操作表
 * ============================================================================ */

static const pmm_ops_t g_buddy_ops = {
    .name         = "buddy",
    .init         = pmm_buddy_init,
    .alloc_page   = pmm_buddy_alloc_page,
    .free_page    = pmm_buddy_free_page,
    .total_pages  = pmm_buddy_total_pages,
    .free_pages   = pmm_buddy_free_pages,
    .managed_base = pmm_buddy_managed_base,
    .page_addr    = pmm_buddy_page_addr,
    .page_is_used = pmm_buddy_page_is_used,
};

const pmm_ops_t* pmm_buddy_get_ops(void) {
    return &g_buddy_ops;
}
