#include <stddef.h>

#include "kmalloc.h"
#include "printk.h"

/*
 * kmalloc.c — 内核堆 dispatch 层（可插拔后端转发）
 *
 * [WHY] 和 pmm.c 一样的模式：所有调用者只调公开 API，
 *   本文件通过 g_heap_ops 转发到当前注册的后端。
 *
 * 当前状态：空壳实现，所有操作返回 NULL/0。
 * 后续 PR 会接入 first-fit 后端。
 */

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
    /* TODO: 从 VMM 分配堆区域，调用 g_heap_ops->init() */
}

void* kmalloc(size_t size) {
    (void)size;
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
