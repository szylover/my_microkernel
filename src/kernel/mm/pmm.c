#include <stddef.h>

#include "pmm.h"
#include "pmm_backends.h"
#include "printk.h"

/*
 * pmm.c — PMM dispatch 层（可插拔后端转发）
 *
 * ============================================================================
 * [WHY] 为什么要有这一层？
 * ============================================================================
 *
 * 这是 PMM 的"前端"：所有调用者（VMM、Shell 命令、将来的 kmalloc）
 * 都只调用 pmm_alloc_page() 等公开函数。
 *
 * 本文件通过 g_pmm_ops 函数指针表，将调用转发到当前注册的后端。
 * 后端可以是 bitmap（默认）、buddy、或任何实现了 pmm_ops_t 的分配器。
 *
 * 切换后端只需在 pmm_init() 之前调用 pmm_register_backend()。
 * 如果没有主动注册，默认使用 bitmap 后端。
 *
 * 这类似于 Linux VFS 的 file_operations 模式：
 *   struct file_operations { .read = xxx_read, .write = xxx_write, ... };
 *   只不过我们的"文件系统"是物理内存分配器。
 */

/* 当前注册的后端（NULL = 尚未注册，pmm_init 会使用默认 bitmap） */
static const pmm_ops_t* g_pmm_ops = NULL;

void pmm_register_backend(const pmm_ops_t* ops) {
    g_pmm_ops = ops;
}

const char* pmm_backend_name(void) {
    return g_pmm_ops ? g_pmm_ops->name : NULL;
}

void pmm_init(void) {
    /*
     * 如果没有主动注册后端，使用 bitmap 作为默认后端。
     *
     * [WHY] 降低使用门槛：调用者不需要知道后端细节，
     *   只要调 pmm_init() 就能工作。
     */
    if (!g_pmm_ops) {
        g_pmm_ops = pmm_bitmap_get_ops();
    }

    printk("[pmm] backend: %s\n", g_pmm_ops->name);
    g_pmm_ops->init();
}

void* pmm_alloc_page(void) {
    if (!g_pmm_ops || !g_pmm_ops->alloc_page) return NULL;
    return g_pmm_ops->alloc_page();
}

void pmm_free_page(void* page) {
    if (!g_pmm_ops || !g_pmm_ops->free_page) return;
    g_pmm_ops->free_page(page);
}

unsigned pmm_total_pages(void) {
    if (!g_pmm_ops || !g_pmm_ops->total_pages) return 0;
    return g_pmm_ops->total_pages();
}

unsigned pmm_free_pages(void) {
    if (!g_pmm_ops || !g_pmm_ops->free_pages) return 0;
    return g_pmm_ops->free_pages();
}

unsigned pmm_managed_base(void) {
    if (!g_pmm_ops || !g_pmm_ops->managed_base) return 0;
    return g_pmm_ops->managed_base();
}

unsigned pmm_page_addr(unsigned page_index) {
    if (!g_pmm_ops || !g_pmm_ops->page_addr) return 0;
    return g_pmm_ops->page_addr(page_index);
}

int pmm_page_is_used(unsigned page_index) {
    if (!g_pmm_ops || !g_pmm_ops->page_is_used) return -1;
    return g_pmm_ops->page_is_used(page_index);
}

