#include <stddef.h>

#include "vma.h"
#include "printk.h"

/*
 * vma.c — VMA dispatch 层（可插拔后端转发）
 *
 * [WHY] 和 pmm.c / kmalloc.c 一样的模式：
 *   所有调用者只调公开 API（vma_add / vma_find / ...），
 *   本文件通过 g_vma_ops 转发到当前注册的后端。
 *
 * 架构：
 *
 *   调用者 → vma_add() → g_vma_ops->add() → 后端实现（如 vma_rbtree.c）
 *
 *   后端切换只需在 vma_init() 前调用 vma_register_backend(&new_ops)。
 */

static const vma_ops_t* g_vma_ops = NULL;
static int g_vma_ready = 0;

void vma_register_backend(const vma_ops_t* ops) {
    g_vma_ops = ops;
}

const char* vma_backend_name(void) {
    return g_vma_ops ? g_vma_ops->name : NULL;
}

void vma_init(void) {
    if (!g_vma_ops) {
        printk("[vma] no backend registered, VMA unavailable\n");
        return;
    }

    printk("[vma] backend: %s\n", g_vma_ops->name);

    g_vma_ops->init();
    g_vma_ready = 1;

    printk("[vma] init ok\n");
}

int vma_add(uint32_t start, uint32_t end, uint32_t flags, const char* name) {
    if (!g_vma_ops || !g_vma_ready) return -1;
    return g_vma_ops->add(start, end, flags, name);
}

int vma_remove(uint32_t start) {
    if (!g_vma_ops || !g_vma_ready) return -1;
    return g_vma_ops->remove(start);
}

const vm_area_t* vma_find(uint32_t addr) {
    if (!g_vma_ops || !g_vma_ready) return NULL;
    return g_vma_ops->find(addr);
}

void vma_dump(void) {
    if (!g_vma_ops || !g_vma_ready) {
        printk("VMA: not initialized\n");
        return;
    }
    g_vma_ops->dump();
}

unsigned vma_count(void) {
    if (!g_vma_ops || !g_vma_ready) return 0;
    return g_vma_ops->count();
}

int vma_is_ready(void) {
    return g_vma_ready;
}
