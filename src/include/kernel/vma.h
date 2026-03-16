#pragma once

/*
 * vma.h — Virtual Memory Area（VMA）管理（可插拔后端接口）
 *
 * [WHY] 为什么需要 VMA？
 *   Page Fault 发生时，内核需要知道故障地址"应不应该"被映射。
 *   VMA 显式注册每个虚拟内存区域的地址范围与权限，
 *   使 #PF handler 能区分"合法的按需分页"与"非法访问"。
 *
 * 架构（和 pmm_ops_t / heap_ops_t 同一套可插拔模式）：
 *
 *   调用者 (kmain.c, vmm.c, idt.c, shell cmds)
 *     │
 *     ▼
 *   vma.h (公开 API)  ── vma_add() / vma_remove() / vma_find() / ...
 *     │
 *     ▼
 *   vma.c (dispatch)  ── 通过 g_vma_ops 函数指针表转发
 *     │
 *     ▼
 *   vma_ops_t 后端实现
 *
 * 地址约定（和 Linux 一致）：
 *   VMA 覆盖 [start, end)，start/end 均页对齐，不允许重叠。
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * VMA 权限标志
 * ============================================================================ */

#define VMA_READ    0x01u   /* 可读 */
#define VMA_WRITE   0x02u   /* 可写 */
#define VMA_EXEC    0x04u   /* 可执行（将来配合 NX 位使用） */

/* ============================================================================
 * VMA 描述结构（公开，只读）
 *
 * 不包含后端内部管理字段，保持接口与实现解耦。
 * ============================================================================ */

typedef struct vm_area {
    uint32_t    start;      /* 起始虚拟地址（包含，页对齐） */
    uint32_t    end;        /* 结束虚拟地址（不包含，页对齐） */
    uint32_t    flags;      /* VMA_READ | VMA_WRITE | VMA_EXEC */
    const char* name;       /* 人类可读名称，如 "direct-map", "kheap" */
} vm_area_t;

/* ============================================================================
 * vma_ops_t — 后端操作表（函数指针）
 * ============================================================================ */

typedef struct vma_ops {
    const char* name;

    void (*init)(void);

    /* add: 0=成功, -1=失败（地址重叠 / 容量满） */
    int (*add)(uint32_t start, uint32_t end, uint32_t flags, const char* name);

    /* remove: 按起始地址匹配, 0=成功, -1=未找到 */
    int (*remove)(uint32_t start);

    /* find: 返回包含 addr 的 VMA，未找到返回 NULL */
    const vm_area_t* (*find)(uint32_t addr);

    /* dump: 按地址排序打印所有 VMA */
    void (*dump)(void);

    /* count: 当前已注册 VMA 数量 */
    unsigned (*count)(void);
} vma_ops_t;

/* ============================================================================
 * 后端注册（在 vma_init 之前调用）
 * ============================================================================ */

void vma_register_backend(const vma_ops_t* ops);

/* 后端声明（具体实现在各自 .c 文件中） */
const vma_ops_t* vma_sorted_array_get_ops(void);  /* 排序数组（早期简单实现） */
const vma_ops_t* vma_rbtree_get_ops(void);         /* 红黑树（Linux 2.4~6.0） */
const vma_ops_t* vma_maple_get_ops(void);          /* maple tree（Linux 6.1+） */

/* ============================================================================
 * 公开 API
 * ============================================================================ */

void  vma_init(void);
int   vma_add(uint32_t start, uint32_t end, uint32_t flags, const char* name);
int   vma_remove(uint32_t start);
const vm_area_t* vma_find(uint32_t addr);
void  vma_dump(void);
unsigned vma_count(void);
int   vma_is_ready(void);
const char* vma_backend_name(void);

#ifdef __cplusplus
}
#endif
