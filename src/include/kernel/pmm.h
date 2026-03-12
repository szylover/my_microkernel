
#pragma once

/*
 * pmm.h — Physical Memory Manager (PMM) — 可插拔后端接口
 *
 * ============================================================================
 * 设计思想
 * ============================================================================
 *
 * [WHY] 为什么要抽象 PMM 后端？
 *   不同的物理内存分配算法（bitmap、buddy、hybrid）各有优缺点。
 *   将算法实现与调用接口解耦，可以：
 *   - 在不改 VMM/Shell/kmalloc 调用方的前提下替换分配算法
 *   - 同时保留多个后端，按需切换（甚至运行时切换）
 *   - 新算法只需实现 pmm_ops_t 并注册即可
 *
 * 架构（类似 Linux 的 struct file_operations）：
 *
 *   调用者 (vmm.c, kmalloc.c, shell cmds)
 *     │
 *     ▼
 *   pmm.h (公开 API)  ── pmm_alloc_page() / pmm_free_page() / ...
 *     │
 *     ▼
 *   pmm.c (dispatch)  ── 通过 g_pmm_ops 函数指针表转发
 *     │
 *     ▼
 *   pmm_ops_t 后端实现
 *     ├── pmm_bitmap.c  (当前: bitmap 分配器)
 *     ├── pmm_buddy.c   (将来: buddy 分配器)
 *     └── ...
 *
 * ============================================================================
 * 使用方式
 * ============================================================================
 *
 * 1. 后端实现 pmm_ops_t 中的所有函数指针
 * 2. 在 pmm_init() 之前调用 pmm_register_backend(&my_ops)
 *    （或在 pmm_init 内部硬编码默认后端）
 * 3. 之后所有 pmm_alloc_page() 等调用自动走注册的后端
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PMM_PAGE_SIZE 4096u

/* ============================================================================
 * pmm_ops_t — 后端操作表（函数指针）
 * ============================================================================
 *
 * [WHY] 每个后端（bitmap / buddy / ...）实现这组函数。
 *   pmm.c 的公开 API 通过这个表做间接调用。
 *
 *   类比 Linux:
 *     struct page_alloc_ops { ... };
 *   类比 VFS:
 *     struct file_operations { .read = ..., .write = ..., };
 */
typedef struct pmm_ops {
    const char* name;       /* 后端名称，用于日志/诊断（如 "bitmap", "buddy"） */

    /*
     * init — 初始化后端
     *
     * 由 pmm_init() 调用。后端应在此函数中：
     *   - 解析 Multiboot2 mmap
     *   - 建立内部数据结构（bitmap / free_list / ...）
     *   - 标记保留区域（内核镜像、mb2 info 等）
     *   - 运行 selftest
     */
    void (*init)(void);

    /* alloc_page — 分配一个 4KiB 物理页，返回物理地址，NULL=OOM */
    void* (*alloc_page)(void);

    /* free_page — 释放一个物理页 */
    void (*free_page)(void* page);

    /* total_pages — 管理的总页数 */
    unsigned (*total_pages)(void);

    /* free_pages — 当前空闲页数 */
    unsigned (*free_pages)(void);

    /* managed_base — 管理区域的物理起始地址，0=未初始化 */
    unsigned (*managed_base)(void);

    /* page_addr — 全局页索引→物理地址，0=无效 */
    unsigned (*page_addr)(unsigned page_index);

    /* page_is_used — 页索引→使用状态: 1=used, 0=free, -1=invalid */
    int (*page_is_used)(unsigned page_index);
} pmm_ops_t;

/* ============================================================================
 * 后端注册
 * ============================================================================ */

/*
 * pmm_register_backend — 注册一个 PMM 后端
 *
 * @param ops  后端操作表（指向静态/全局数据，PMM 不做拷贝）
 *
 * [WHY] 在 pmm_init() 之前调用。
 *   如果不调用，pmm_init() 使用编译时默认后端（bitmap）。
 */
void pmm_register_backend(const pmm_ops_t* ops);

/* ============================================================================
 * 公开 API（调用者使用，不变）
 * ============================================================================ */

void pmm_init(void);
void* pmm_alloc_page(void);
void pmm_free_page(void* page);
unsigned pmm_total_pages(void);
unsigned pmm_free_pages(void);
unsigned pmm_managed_base(void);
unsigned pmm_page_addr(unsigned page_index);
int pmm_page_is_used(unsigned page_index);

/* 返回当前后端名称（ "bitmap" / "buddy" / ...），NULL=未注册 */
const char* pmm_backend_name(void);

#ifdef __cplusplus
}
#endif

