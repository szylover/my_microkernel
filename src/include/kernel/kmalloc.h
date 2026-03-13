#pragma once

/*
 * kmalloc.h — 内核堆分配器（可插拔后端接口）
 *
 * [WHY] 为什么要抽象堆分配后端？
 *   和 PMM (pmm_ops_t) 一样的思路：将算法实现与调用接口解耦。
 *   Stage 8 先用 first-fit 空闲链表，将来可以替换为 slab，
 *   调用方完全不需要改动。
 *
 * 架构：
 *
 *   调用者 (VMA, 进程管理, shell cmds)
 *     │
 *     ▼
 *   kmalloc.h (公开 API)  ── kmalloc() / kfree() / kheap_get_stats()
 *     │
 *     ▼
 *   kmalloc.c (dispatch)  ── 通过 g_heap_ops 函数指针表转发
 *     │
 *     ▼
 *   heap_ops_t 后端实现
 *     ├── heap_first_fit.c  (Stage 8: first-fit 空闲链表)
 *     └── heap_slab.c       (将来: slab 分配器)
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 堆统计信息
 * ============================================================================ */

typedef struct kheap_stats {
    size_t heap_size;       /* 堆总大小（字节） */
    size_t used_bytes;      /* 已分配字节数（含元数据开销） */
    size_t free_bytes;      /* 空闲字节数 */
    unsigned alloc_count;   /* 当前活跃分配数 */
    unsigned free_count;    /* 空闲块数 */
} kheap_stats_t;

/* ============================================================================
 * heap_ops_t — 后端操作表（函数指针）
 *
 * [WHY] 每个后端（first-fit / slab / ...）实现这组函数。
 *   kmalloc.c 通过这个表做间接调用，类比 PMM 的 pmm_ops_t。
 * ============================================================================ */

typedef struct heap_ops {
    const char* name;       /* 后端名称（如 "first_fit", "slab"） */

    /*
     * init — 初始化堆后端
     * @param start  堆区域虚拟起始地址
     * @param size   堆区域大小（字节，4KiB 对齐）
     */
    void (*init)(void* start, size_t size);

    /*
     * alloc — 分配内存块
     * @param size  请求字节数（> 0）
     * @return      至少 8 字节对齐的指针，NULL=OOM
     */
    void* (*alloc)(size_t size);

    /*
     * free — 释放内存块
     * @param ptr  由 alloc 返回的指针（NULL 安全）
     */
    void (*free)(void* ptr);

    /*
     * stats — 获取堆统计信息
     * @param out  输出结构体
     */
    void (*stats)(kheap_stats_t* out);
} heap_ops_t;

/* ============================================================================
 * 后端注册
 * ============================================================================ */

void kmalloc_register_backend(const heap_ops_t* ops);

/* first-fit 空闲链表后端（Stage 8 默认） */
const heap_ops_t* heap_first_fit_get_ops(void);

/* slab 分配器后端（配合 buddy PMM 更高效） */
const heap_ops_t* heap_slab_get_ops(void);

/* ============================================================================
 * 公开 API
 * ============================================================================ */

void  kmalloc_init(void);
void* kmalloc(size_t size);
void  kfree(void* ptr);
void  kheap_get_stats(kheap_stats_t* out);
const char* kmalloc_backend_name(void);

#ifdef __cplusplus
}
#endif
