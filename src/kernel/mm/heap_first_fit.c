#include <stdint.h>
#include <stddef.h>

#include "kmalloc.h"
#include "vmm.h"
#include "pmm.h"
#include "printk.h"

/*
 * heap_first_fit.c — First-Fit 空闲链表内核堆后端
 *
 * ============================================================================
 * 核心思路
 * ============================================================================
 *
 * 把一段连续虚拟内存切成若干"块"(block)，每块前面有个小 header，
 * 所有块串成单向链表：
 *
 *   free_list
 *   │
 *   ▼
 *   +------+--------+   +------+--------+   +------+--------+
 *   | hdr  |  data  |-->| hdr  |  data  |-->| hdr  |  data  | --> NULL
 *   | free |  ...   |   | used |  ...   |   | free |  ...   |
 *   +------+--------+   +------+--------+   +------+--------+
 *
 * alloc: 从头遍历，找第一个 is_free && size >= 请求 的块（first-fit）
 * free:  把块标记为 free，然后合并相邻的 free 块
 *
 * ============================================================================
 * 堆的虚拟地址区间
 * ============================================================================
 *
 *   KHEAP_START (0xE0000000)                      KHEAP_END (0xF0000000)
 *   │                                             │
 *   ▼                                             ▼
 *   +--------------------------+··················+
 *   | 已提交区域 (有物理页)     | 未映射 (PTE=0)  |
 *   | free_list 在这里管理     |                  |
 *   +--------------------------+··················+
 *                              ▲
 *                              heap_brk
 *
 * heap_brk 以下的虚拟页已经映射了物理页，可以安全读写。
 * 当 alloc 发现空间不够，调 heap_grow() 推进 heap_brk。
 */

/* ============================================================================
 * 块头结构 (block_header_t)
 *
 * 放在每个块的最前面，记录这块内存的元数据。
 *
 * alloc 返回的指针 = (uint8_t*)header + HEADER_SIZE（跳过头部）
 * free 时从指针往回退 HEADER_SIZE 就找到 header。
 *
 *   header 指针
 *   │
 *   ▼
 *   +------------------+----------------------------------+
 *   | block_header_t   | 用户数据区 (header->size 字节)   |
 *   | {size, is_free,  |                                  |
 *   |  next}           |   ← kmalloc 返回这里的地址       |
 *   +------------------+----------------------------------+
 *   |<-- HEADER_SIZE ->|<---------- size ---------------->|
 * ============================================================================ */

typedef struct block_header {
    size_t size;                    /* 用户数据区大小（字节，不含 header） */
    int is_free;                    /* 0=已分配, 1=空闲 */
    struct block_header* next;      /* 下一个块 */
    struct block_header* prev;      /* 上一个块（双向链表，方便向前合并） */
} block_header_t;

/*
 * HEADER_SIZE — 对齐到 8 字节
 *
 * [WHY] sizeof(block_header_t) 在 32 位下是 16 字节 (4+4+4+4)。
 *   刚好 8 字节对齐，不需要额外 padding。
 */
#define HEADER_SIZE ((sizeof(block_header_t) + 7u) & ~7u)

/* 将 size 向上对齐到 8 字节 */
#define ALIGN8(x) (((x) + 7u) & ~7u)

/* ============================================================================
 * 堆状态变量
 * ============================================================================ */

static uint32_t heap_start = 0;     /* 堆虚拟起始地址 (= KHEAP_START) */
static uint32_t heap_brk   = 0;     /* 已提交区域的上界 */
static uint32_t heap_end   = 0;     /* 堆虚拟上限 (= KHEAP_END) */

static block_header_t* free_list = NULL;  /* 链表头 */

/* ============================================================================
 * merge_with_next — 将 block 和 block->next 合并为一个块
 *
 * [WHY] alloc 拆分和 free 合并都需要“吃掉下一个块”的操作，
 *   抽成辅助函数避免重复代码。
 *
 *   合并前:
 *     +------+------+   +------+------+
 *     | hdr  | data |-->| hdr  | data |--> ...
 *     | block|      |   |victim|      |
 *     +------+------+   +------+------+
 *
 *   合并后:
 *     +------+----------------------------+
 *     | hdr  |       更大的 data           |--> ...
 *     | block|                            |
 *     +------+----------------------------+
 *
 * 前置条件: block->next != NULL
 * ============================================================================ */
static void merge_with_next(block_header_t* block) {
    block_header_t* victim = block->next;
    block->size += HEADER_SIZE + victim->size;
    block->next = victim->next;
    if (victim->next) {
        victim->next->prev = block;
    }
}

/* ============================================================================
 * heap_grow — 按需扩展堆
 *
 * 从 PMM 申请物理页，映射到 heap_brk 上方，推进 heap_brk。
 *
 * @param min_bytes  至少需要的字节数（内部向上对齐到整页）
 * @return           0 成功, -1 失败
 * ============================================================================ */
static int heap_grow(size_t min_bytes) {
    size_t pages = (min_bytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        if (heap_brk >= heap_end) {
            printk("[heap] FATAL: heap limit reached (brk=0x%08x)\n", heap_brk);
            return -1;
        }

        uint32_t phys = (uint32_t)(uintptr_t)pmm_alloc_page();
        if (!phys) {
            printk("[heap] FATAL: PMM OOM during heap_grow\n");
            return -1;
        }

        if (vmm_map_page(heap_brk, phys, PTE_PRESENT | PTE_WRITABLE) != 0) {
            pmm_free_page((void*)(uintptr_t)phys);
            printk("[heap] FATAL: vmm_map_page failed at 0x%08x\n", heap_brk);
            return -1;
        }

        heap_brk += VMM_PAGE_SIZE;
    }

    return 0;
}

/* ============================================================================
 * heap_first_fit_init — 初始化堆后端
 *
 * kmalloc_init() 已经把 [start, start+size) 映射好了物理页，
 * 我们只需要在这段内存上建立初始的空闲链表。
 *
 * 初始状态：整个区域 = 1 个大的空闲块。
 * ============================================================================ */
static void heap_first_fit_init(void* start, size_t size) {
    heap_start = (uint32_t)(uintptr_t)start;
    heap_brk   = heap_start + size;
    heap_end   = KHEAP_END;

    /* 整个已提交区域作为一个空闲块 */
    free_list = (block_header_t*)(uintptr_t)heap_start;
    free_list->size    = size - HEADER_SIZE;
    free_list->is_free = 1;
    free_list->next    = NULL;
    free_list->prev    = NULL;
}

/* ============================================================================
 * heap_first_fit_alloc — 分配内存
 *
 * TODO: 请你实现这个函数！算法步骤如下：
 *
 * 1. 对齐请求大小:
 *      size = ALIGN8(size);
 *
 * 2. 遍历链表找 first-fit:
 *      从 free_list 开始，找第一个 is_free==1 且 block->size >= size 的块。
 *
 * 3. 找到后，尝试拆分:
 *      如果 block->size - size > HEADER_SIZE + 8（剩余空间够放一个新块），
 *      就把当前块拆成两个：
 *
 *        拆分前：
 *        +--------+--------- block->size -------------------+
 *        | header |                大空闲块                  |
 *        +--------+-----------------------------------------+
 *
 *        拆分后：
 *        +--------+-- size --+--------+-- remaining_size ---+
 *        | header | 已分配   | new_hdr|     新空闲块         |
 *        +--------+----------+--------+---------------------+
 *
 *      new_block 地址 = (uint8_t*)block + HEADER_SIZE + size
 *      new_block->size = block->size - size - HEADER_SIZE
 *      new_block->is_free = 1
 *      new_block->next = block->next
 *      block->size = size
 *      block->next = new_block
 *
 *      如果剩余空间不够拆（<= HEADER_SIZE + 8），就整块给出去，不拆分。
 *
 * 4. 标记已分配:
 *      block->is_free = 0;
 *
 * 5. 返回用户指针:
 *      return (void*)((uint8_t*)block + HEADER_SIZE);
 *
 * 6. 如果遍历完没找到:
 *      调 heap_grow(size + HEADER_SIZE) 扩展堆，
 *      然后把新空间接入链表（参考下面的提示），再递归调用自己。
 *
 *   --- 扩展后接入链表的提示 ---
 *   找到链表最后一个块 (last)，算出 last 的末尾地址:
 *     last_end = (uint32_t)last + HEADER_SIZE + last->size
 *
 *   如果 last->is_free:
 *     直接扩大 last->size: last->size += (heap_brk - last_end)
 *   否则:
 *     在 last_end 处创建新的空闲块，挂到 last->next
 *
 * @return  指针 (8 字节对齐), NULL = OOM
 * ============================================================================ */
static void* heap_first_fit_alloc(size_t size) {
    size_t orig_size = size;
    size = ALIGN8(size);
    printk("[heap] alloc: req=%u aligned=%u\n", (unsigned)orig_size, (unsigned)size);

    for (block_header_t* curr = free_list; curr; curr = curr->next) {
        if (curr->is_free && curr->size >= size) {
            printk("[heap] alloc: found block @ 0x%08x (size=%u, need=%u)\n",
                   (unsigned)(uintptr_t)curr, (unsigned)curr->size, (unsigned)size);
            if (curr->size - size > HEADER_SIZE + 8) {
                /* 拆分块 */
                block_header_t* new_block = (block_header_t*)((uint8_t*)curr + HEADER_SIZE + size);
                new_block->size = curr->size - size - HEADER_SIZE;
                new_block->is_free = 1;
                new_block->next = curr->next;
                new_block->prev = curr;
                if (curr->next) {
                    curr->next->prev = new_block;
                }

                curr->size = size;
                curr->next = new_block;
                printk("[heap] alloc: split -> used %u + free %u @ 0x%08x\n",
                       (unsigned)size, (unsigned)new_block->size,
                       (unsigned)(uintptr_t)new_block);
            } else {
                printk("[heap] alloc: no split (remaining %u <= hdr+8)\n",
                       (unsigned)(curr->size - size));
            }
            curr->is_free = 0;
            void* result = (void*)((uint8_t*)curr + HEADER_SIZE);
            printk("[heap] alloc: return 0x%08x\n", (unsigned)(uintptr_t)result);
            return result;
        }
    }

    printk("[heap] alloc: no fit found, returning NULL\n");
    return NULL;
}

/* ============================================================================
 * heap_first_fit_free — 释放内存并合并相邻空闲块
 *
 * TODO: 请你实现这个函数！算法步骤如下：
 *
 * 1. 从用户指针反推 header:
 *      block_header_t* block = (block_header_t*)((uint8_t*)ptr - HEADER_SIZE);
 *
 * 2. 合法性检查（可选但推荐）:
 *      block 地址应该在 [heap_start, heap_brk) 范围内。
 *
 * 3. 标记为空闲:
 *      block->is_free = 1;
 *
 * 4. 合并相邻空闲块（forward coalescing）:
 *      从 free_list 开始遍历，如果 curr->is_free && curr->next->is_free:
 *        curr 吃掉 curr->next：
 *          curr->size += HEADER_SIZE + curr->next->size
 *          curr->next = curr->next->next
 *        注意吃完后不要移动 curr，可能还能继续吃下一个。
 *
 *    图示：
 *      合并前：
 *        +------+------+   +------+------+
 *        | hdr  | data |-->| hdr  | data |--> ...
 *        | FREE |      |   | FREE |      |
 *        +------+------+   +------+------+
 *
 *      合并后：
 *        +------+----------------------------+
 *        | hdr  |       更大的 data           |--> ...
 *        | FREE |                            |
 *        +------+----------------------------+
 * ============================================================================ */
static void heap_first_fit_free(void* ptr) {
    if(!ptr) {
        return;
    }

    block_header_t* block = (block_header_t*)((uint8_t*)ptr - HEADER_SIZE);
    if ((uint32_t)(uintptr_t)block < heap_start || (uint32_t)(uintptr_t)block >= heap_brk) {
        printk("[heap] WARNING: attempt to free invalid pointer 0x%08x\n", (unsigned)(uintptr_t)ptr);
        return;
    }

    printk("[heap] free: ptr=0x%08x block=0x%08x size=%u\n",
           (unsigned)(uintptr_t)ptr,
           (unsigned)(uintptr_t)block,
           (unsigned)block->size);

    block->is_free = 1;
    
    unsigned merged = 0;

    /* 向后合并：吃掉 block->next（如果它也是 free） */
    while (block->next && block->next->is_free) {
        printk("[heap] free: merge forward 0x%08x + 0x%08x\n",
               (unsigned)(uintptr_t)block, (unsigned)(uintptr_t)block->next);
        merge_with_next(block);
        merged++;
    }

    /* 向前合并：被 block->prev 吃掉（如果它也是 free） */
    if (block->prev && block->prev->is_free) {
        printk("[heap] free: merge backward 0x%08x + 0x%08x\n",
               (unsigned)(uintptr_t)block->prev, (unsigned)(uintptr_t)block);
        merge_with_next(block->prev);
        merged++;
    }

    printk("[heap] free: done (merged %u adjacent blocks)\n", merged);
}

/* ============================================================================
 * heap_first_fit_stats — 收集堆统计信息（已实现，供 heap status 命令用）
 * ============================================================================ */
static void heap_first_fit_stats(kheap_stats_t* out) {
    out->heap_size   = heap_brk - heap_start;
    out->used_bytes  = 0;
    out->free_bytes  = 0;
    out->alloc_count = 0;
    out->free_count  = 0;

    block_header_t* curr = free_list;
    while (curr) {
        if (curr->is_free) {
            out->free_bytes += curr->size;
            out->free_count++;
        } else {
            out->used_bytes += curr->size + HEADER_SIZE;
            out->alloc_count++;
        }
        curr = curr->next;
    }
}

/* ============================================================================
 * 后端操作表 — 注册到 kmalloc dispatch 层
 * ============================================================================ */

static const heap_ops_t heap_first_fit_ops = {
    .name  = "first_fit",
    .init  = heap_first_fit_init,
    .alloc = heap_first_fit_alloc,
    .free  = heap_first_fit_free,
    .stats = heap_first_fit_stats,
};

const heap_ops_t* heap_first_fit_get_ops(void) {
    return &heap_first_fit_ops;
}
