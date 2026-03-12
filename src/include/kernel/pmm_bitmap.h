#pragma once

/*
 * pmm_bitmap.h — bitmap PMM 后端内部头文件
 *
 * [WHY] 仅由 pmm.c dispatch 层 include，用于获取 bitmap 后端的 ops 表。
 *   其他代码不应直接 include 此文件，而是通过 pmm.h 的公开 API。
 */

#include "pmm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 返回 bitmap 后端的 pmm_ops_t 操作表指针 */
const pmm_ops_t* pmm_bitmap_get_ops(void);

#ifdef __cplusplus
}
#endif
