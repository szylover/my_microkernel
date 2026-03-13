#include <stdint.h>
#include <stddef.h>

#include "kmalloc.h"
#include "vmm.h"
#include "pmm.h"
#include "printk.h"

/*
 * heap_slab.c — Slab 分配器内核堆后端
 *
 * ============================================================================
 * [WHY] 为什么要 slab？
 * ============================================================================
 *
 * first-fit 空闲链表每次 alloc 要 O(n) 遍历，碎片化严重。
 * slab 分配器预先把内存切成固定大小的 "slot"，同大小的 alloc 直接 O(1) 取。
 *
 * 核心思想（来自 Solaris / Linux）：
 *   - 维护几种固定大小的 "cache"（32B, 64B, 128B, ...）
 *   - 每个 cache 管理若干 "slab"，每个 slab = 一页(4KiB) 物理内存
 *   - slab 内部被等分成 N 个 object slot，用 bitmap 跟踪哪些空闲
 *   - alloc: 找到对应 cache → 从 partial slab 取一个空闲 slot
 *   - free:  从指针算出属于哪个 slab → bitmap 置位归还
 *
 * ============================================================================
 * 一个 slab 的内存布局（一页 4096 字节）
 * ============================================================================
 *
 *   page_start (4KiB 对齐)
 *   │
 *   ▼
 *   +------------------+--------+--------+--------+-----+--------+
 *   | slab_header_t    | obj[0] | obj[1] | obj[2] | ... | obj[N] |
 *   | (元数据+bitmap)  | 32/64/ | 32/64/ | 32/64/ |     |        |
 *   +------------------+--------+--------+--------+-----+--------+
 *   |<- SLAB_HDR_SIZE->|<------------ objs_per_slab ------------>|
 *
 *   [WHY] 把 header 放在页头而非单独分配：
 *     - 不需要额外的 kmalloc 来存 metadata（避免鸡生蛋）
 *     - kfree 时 `ptr & ~0xFFF` 直接找到 header，O(1)
 *     - 代价：每页损失 ~40 字节（可接受）
 *
 * ============================================================================
 * Cache 结构
 * ============================================================================
 *
 *   caches[0] (32B)     caches[1] (64B)     caches[6] (2048B)
 *   ┌─────────┐         ┌─────────┐         ┌─────────┐
 *   │ partial ─┼──→slab──→slab     │ partial │         │ partial │
 *   │ full    ─┼──→slab  │ full    │         │ full    │
 *   └─────────┘         └─────────┘         └─────────┘
 *
 *   partial: 有空闲 slot 的 slab 链表（alloc 从这里取）
 *   full:    全满的 slab 链表（free 时可能移回 partial）
 */

/* ============================================================================
 * 常量与 cache 大小
 * ============================================================================ */

/*
 * [WHY] 7 级 cache 覆盖 32B ~ 2048B：
 *   内核大部分 kmalloc 请求都在这个范围。
 *   kmalloc(50) → 找 cache-64（向上取最近的 cache 大小）。
 *   > 2048B 的请求返回 NULL（将来可扩展为直接分配整页）。
 */
#define NUM_CACHES 7
static const unsigned cache_sizes[NUM_CACHES] = {
    32, 64, 128, 256, 512, 1024, 2048
};

/* ============================================================================
 * slab_header_t — 放在每个 slab 页的开头
 * ============================================================================ */

typedef struct slab_header {
    struct slab_header* next;       /* 同一 cache 的下一个 slab */
    unsigned cache_idx;             /* 属于哪个 cache (0..6) */
    unsigned obj_size;              /* object 大小（冗余存储，方便 free） */
    unsigned capacity;              /* 本 slab 能放多少个 object */
    unsigned free_count;            /* 当前空闲 slot 数 */
    /*
     * free_bitmap — 128 位 = 4 × uint32_t
     *
     * [WHY] 最小 obj_size=32，一页 (4096-40)/32 ≈ 126 个 object，
     *   需要 126 位 → 128 位（4 个 uint32_t）足够。
     *   bit=1 表示空闲，bit=0 表示已分配。
     */
    uint32_t free_bitmap[4];
} slab_header_t;

/*
 * SLAB_HDR_SIZE — header 大小（8 字节对齐）
 *
 * [WHY] 保证 object[0] 的起始地址 8 字节对齐。
 *   sizeof(slab_header_t) = 40 字节（已经是 8 的倍数）。
 */
#define SLAB_HDR_SIZE ((sizeof(slab_header_t) + 7u) & ~7u)

/* ============================================================================
 * slab_cache_t — 管理一种固定大小的 object
 * ============================================================================ */

typedef struct {
    unsigned obj_size;              /* 每个 object 的大小 */
    slab_header_t* partial;         /* 有空闲 slot 的 slab 链表 */
    slab_header_t* full;            /* 满的 slab 链表 */
} slab_cache_t;

static slab_cache_t caches[NUM_CACHES];

/* ============================================================================
 * 虚拟地址管理
 *
 * [WHY] slab 需要为每个新 slab 分配一页虚拟地址。
 *   使用和 first-fit 相同的 KHEAP 区域，但用 bump allocator 按页推进。
 *
 *   slab_brk:       下一个可用的虚拟页地址
 *   slab_committed:  已映射物理页的上界（kmalloc_init 预映射的）
 * ============================================================================ */

static uint32_t slab_brk       = 0;
static uint32_t slab_committed = 0;

/* ============================================================================
 * bitmap 辅助函数
 * ============================================================================ */

/*
 * 在 bitmap 中找第一个置位的 bit（= 第一个空闲 slot）
 * @return  slot index，-1 = 没有空闲
 */
static int bitmap_find_first_set(const uint32_t* bm, unsigned bits) {
    unsigned words = (bits + 31u) / 32u;
    for (unsigned w = 0; w < words; w++) {
        if (bm[w] == 0) continue;
        for (unsigned b = 0; b < 32u; b++) {
            unsigned idx = w * 32u + b;
            if (idx >= bits) return -1;
            if (bm[w] & (1u << b)) return (int)idx;
        }
    }
    return -1;
}

static void bitmap_clear_bit(uint32_t* bm, unsigned idx) {
    bm[idx / 32u] &= ~(1u << (idx % 32u));
}

static void bitmap_set_bit(uint32_t* bm, unsigned idx) {
    bm[idx / 32u] |= (1u << (idx % 32u));
}

/* ============================================================================
 * slab_get_page — 为新 slab 分配一页虚拟内存
 *
 * [WHY] kmalloc_init 预映射了 16 页，用完后按需从 PMM 拿新页。
 *   slab_brk < slab_committed → 页已映射，直接用
 *   slab_brk >= slab_committed → 需要 pmm_alloc + vmm_map
 * ============================================================================ */
static void* slab_get_page(void) {
    if (slab_brk >= KHEAP_END) {
        printk("[slab] FATAL: heap limit reached\n");
        return NULL;
    }

    uint32_t virt = slab_brk;

    if (slab_brk >= slab_committed) {
        /* 超出预映射范围，需要映射新物理页 */
        uint32_t phys = (uint32_t)(uintptr_t)pmm_alloc_page();
        if (!phys) {
            printk("[slab] FATAL: PMM OOM\n");
            return NULL;
        }
        if (vmm_map_page(virt, phys, PTE_PRESENT | PTE_WRITABLE) != 0) {
            pmm_free_page((void*)(uintptr_t)phys);
            printk("[slab] FATAL: vmm_map_page failed at 0x%08x\n", virt);
            return NULL;
        }
        slab_committed += VMM_PAGE_SIZE;
    }

    slab_brk += VMM_PAGE_SIZE;
    return (void*)(uintptr_t)virt;
}

/* ============================================================================
 * slab_create — 创建一个新 slab
 *
 * 分配一页，在页头写入 slab_header，bitmap 全部置 1（全空闲）。
 * ============================================================================ */
static slab_header_t* slab_create(unsigned cache_idx) {
    void* page = slab_get_page();
    if (!page) return NULL;

    slab_header_t* hdr = (slab_header_t*)page;
    unsigned obj_size = caches[cache_idx].obj_size;
    unsigned capacity = (VMM_PAGE_SIZE - (unsigned)SLAB_HDR_SIZE) / obj_size;
    if (capacity > 128u) capacity = 128u;  /* bitmap 上限 */

    hdr->next       = NULL;
    hdr->cache_idx  = cache_idx;
    hdr->obj_size   = obj_size;
    hdr->capacity   = capacity;
    hdr->free_count = capacity;

    /* 清零 bitmap 再逐位置 1（空闲） */
    hdr->free_bitmap[0] = 0;
    hdr->free_bitmap[1] = 0;
    hdr->free_bitmap[2] = 0;
    hdr->free_bitmap[3] = 0;
    for (unsigned i = 0; i < capacity; i++) {
        bitmap_set_bit(hdr->free_bitmap, i);
    }

    return hdr;
}

/* ============================================================================
 * find_cache — 根据请求大小找对应的 cache
 *
 * [WHY] kmalloc(50) → 向上取到 cache-64。
 *   内部碎片 = 64 - 50 = 14 字节，可接受。
 *
 * @return  cache index (0..6)，-1 = 太大
 * ============================================================================ */
static int find_cache(size_t size) {
    for (int i = 0; i < NUM_CACHES; i++) {
        if (size <= cache_sizes[i]) return i;
    }
    return -1;
}

/* ============================================================================
 * heap_slab_init — 初始化 slab 后端
 *
 * kmalloc_init() 已预映射 [start, start+size)，slab 从这里开始分配页。
 * ============================================================================ */
static void heap_slab_init(void* start, size_t size) {
    slab_brk       = (uint32_t)(uintptr_t)start;
    slab_committed = slab_brk + size;

    for (int i = 0; i < NUM_CACHES; i++) {
        caches[i].obj_size = cache_sizes[i];
        caches[i].partial  = NULL;
        caches[i].full     = NULL;
    }
}

/* ============================================================================
 * heap_slab_alloc — 分配内存
 *
 * 流程：
 *   1. find_cache(size) → 找到 cache-N
 *   2. 从 cache->partial 取第一个 slab
 *   3. 如果没有 partial slab → slab_create 创建新的
 *   4. bitmap 找第一个空闲 slot → 清除 bit → 返回 object 地址
 *   5. 如果 slab 满了 → 移到 full 链表
 * ============================================================================ */
static void* heap_slab_alloc(size_t size) {
    if (size == 0) return NULL;

    int ci = find_cache(size);
    if (ci < 0) {
        printk("[slab] alloc: size %u exceeds max cache %u\n",
               (unsigned)size, cache_sizes[NUM_CACHES - 1]);
        return NULL;
    }

    slab_cache_t* cache = &caches[ci];

    /* 确保有 partial slab */
    if (!cache->partial) {
        slab_header_t* new_slab = slab_create((unsigned)ci);
        if (!new_slab) return NULL;
        cache->partial = new_slab;
    }

    slab_header_t* slab = cache->partial;

    /* 找第一个空闲 slot */
    int slot = bitmap_find_first_set(slab->free_bitmap, slab->capacity);
    if (slot < 0) return NULL;  /* 不应该发生（partial slab 一定有空闲） */

    /* 标记为已分配 */
    bitmap_clear_bit(slab->free_bitmap, (unsigned)slot);
    slab->free_count--;

    /*
     * 计算 object 地址：
     *   slab 页起始 + header 大小 + slot * obj_size
     */
    void* obj = (void*)((uint8_t*)slab + SLAB_HDR_SIZE + (unsigned)slot * slab->obj_size);

    /* 如果 slab 满了，从 partial 移到 full */
    if (slab->free_count == 0) {
        cache->partial = slab->next;
        slab->next = cache->full;
        cache->full = slab;
    }

    return obj;
}

/* ============================================================================
 * heap_slab_free — 释放内存
 *
 * 流程：
 *   1. ptr & ~0xFFF → slab 页起始 = slab_header
 *   2. 算出 slot index = (ptr - 页起始 - header) / obj_size
 *   3. bitmap 置位（归还 slot）
 *   4. 如果 slab 从 full 变成 partial → 移回 partial 链表
 * ============================================================================ */
static void heap_slab_free(void* ptr) {
    if (!ptr) return;

    /*
     * [WHY] ptr & ~0xFFF 能得到 slab header：
     *   每个 slab 占整页，页起始地址 4KiB 对齐。
     *   slab_header 就在页的最开头。
     */
    slab_header_t* slab = (slab_header_t*)((uintptr_t)ptr & ~(VMM_PAGE_SIZE - 1u));

    /* 算出 slot index */
    unsigned offset = (unsigned)((uint8_t*)ptr - (uint8_t*)slab) - (unsigned)SLAB_HDR_SIZE;
    unsigned slot = offset / slab->obj_size;

    if (slot >= slab->capacity) {
        printk("[slab] WARNING: invalid free 0x%08x (slot=%u >= cap=%u)\n",
               (unsigned)(uintptr_t)ptr, slot, slab->capacity);
        return;
    }

    /* 标记为空闲 */
    unsigned was_full = (slab->free_count == 0);
    bitmap_set_bit(slab->free_bitmap, slot);
    slab->free_count++;

    /*
     * [WHY] slab 从 full 变成有空闲时，需要移回 partial 链表，
     *   否则下次 alloc 找不到它。
     */
    if (was_full) {
        slab_cache_t* cache = &caches[slab->cache_idx];

        /* 从 full 链表摘除 */
        slab_header_t** pp = &cache->full;
        while (*pp && *pp != slab) pp = &(*pp)->next;
        if (*pp) *pp = slab->next;

        /* 插入 partial 链表头 */
        slab->next = cache->partial;
        cache->partial = slab;
    }
}

/* ============================================================================
 * heap_slab_stats — 收集堆统计信息
 * ============================================================================ */
static void heap_slab_stats(kheap_stats_t* out) {
    out->heap_size   = slab_brk > KHEAP_START ? (slab_brk - KHEAP_START) : 0;
    out->used_bytes  = 0;
    out->free_bytes  = 0;
    out->alloc_count = 0;
    out->free_count  = 0;

    for (int i = 0; i < NUM_CACHES; i++) {
        for (slab_header_t* s = caches[i].partial; s; s = s->next) {
            unsigned used = s->capacity - s->free_count;
            out->alloc_count += used;
            out->used_bytes  += used * s->obj_size;
            out->free_count  += s->free_count;
            out->free_bytes  += s->free_count * s->obj_size;
        }
        for (slab_header_t* s = caches[i].full; s; s = s->next) {
            out->alloc_count += s->capacity;
            out->used_bytes  += s->capacity * s->obj_size;
        }
    }
}

/* ============================================================================
 * 后端操作表
 * ============================================================================ */

static const heap_ops_t heap_slab_ops = {
    .name  = "slab",
    .init  = heap_slab_init,
    .alloc = heap_slab_alloc,
    .free  = heap_slab_free,
    .stats = heap_slab_stats,
};

const heap_ops_t* heap_slab_get_ops(void) {
    return &heap_slab_ops;
}
