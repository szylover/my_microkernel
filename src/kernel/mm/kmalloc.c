#include <stddef.h>

#include "kmalloc.h"
#include "vmm.h"
#include "pmm.h"
#include "printk.h"

/*
 * kmalloc.c — 内核堆 dispatch 层（可插拔后端转发）
 *
 * [WHY] 和 pmm.c 一样的模式：所有调用者只调公开 API，
 *   本文件通过 g_heap_ops 转发到当前注册的后端。
 *
 * kmalloc_init 负责：
 *   1. 在 KHEAP_START 处映射初始物理页
 *   2. 调用后端 init(start, initial_size)
 */

/* 初始堆大小: 16 页 = 64 KiB （后端会按需 heap_grow 扩展） */
#define KHEAP_INITIAL_PAGES 16u

static const heap_ops_t* g_heap_ops = NULL;

void kmalloc_register_backend(const heap_ops_t* ops) {
    g_heap_ops = ops;
}

const char* kmalloc_backend_name(void) {
    return g_heap_ops ? g_heap_ops->name : NULL;
}

void kmalloc_init(void) {
    if (!g_heap_ops) {
        printk("[kmalloc] no backend registered, heap unavailable\n");
        return;
    }

    printk("[kmalloc] backend: %s\n", g_heap_ops->name);

    /*
     * 在 KHEAP_START 处映射初始物理页
     *
     * [WHY] KHEAP_START (0xE0000000) 目前没有物理页支撑，
     *   访问会触发 #PF。这里逐页：PMM 分配物理页 → VMM 建立映射。
     *   映射完成后 [KHEAP_START, KHEAP_START + initial_size) 可安全读写。
     */
    size_t initial_size = KHEAP_INITIAL_PAGES * VMM_PAGE_SIZE;

    for (unsigned i = 0; i < KHEAP_INITIAL_PAGES; i++) {
        uint32_t virt = KHEAP_START + i * VMM_PAGE_SIZE;
        uint32_t phys = (uint32_t)(uintptr_t)pmm_alloc_page();
        if (!phys) {
            printk("[kmalloc] FATAL: PMM OOM at page %u\n", i);
            return;
        }
        if (vmm_map_page(virt, phys, PTE_PRESENT | PTE_WRITABLE) != 0) {
            pmm_free_page((void*)(uintptr_t)phys);
            printk("[kmalloc] FATAL: map failed at 0x%08x\n", virt);
            return;
        }
    }

    /* 调后端 init — 后端在已映射区域上建立空闲链表 */
    g_heap_ops->init((void*)(uintptr_t)KHEAP_START, initial_size);

    printk("[kmalloc] heap at 0x%08x, initial %u KiB, max %u MiB\n",
           KHEAP_START,
           (unsigned)(initial_size / 1024u),
           (unsigned)(KHEAP_MAX_SIZE / (1024u * 1024u)));
}

void* kmalloc(size_t size) {
    if (!g_heap_ops) return NULL;
    return g_heap_ops->alloc(size);
}

void kfree(void* ptr) {
    if (!g_heap_ops) return;
    g_heap_ops->free(ptr);
}

void kheap_get_stats(kheap_stats_t* out) {
    if (!g_heap_ops || !out) return;
    g_heap_ops->stats(out);
}
