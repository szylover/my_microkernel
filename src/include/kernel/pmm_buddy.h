#pragma once

/*
 * pmm_buddy.h — buddy PMM 后端内部头文件
 *
 * [WHY] 仅由 pmm.c dispatch 层或 kmain.c include，用于注册 buddy 后端。
 *   其他代码不应直接 include 此文件，而是通过 pmm.h 的公开 API。
 */

#include "pmm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 返回 buddy 后端的 pmm_ops_t 操作表指针 */
const pmm_ops_t* pmm_buddy_get_ops(void);

#ifdef __cplusplus
}
#endif
