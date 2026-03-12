#pragma once

/*
 * pmm_backends.h — PMM 后端注册表
 *
 * 所有可用的 PMM 后端在此统一声明。
 * kmain.c 或 pmm.c 只需 include 这一个文件即可访问任意后端。
 *
 * [WHY] 为什么合并成一个头文件？
 *   - 每个后端都只导出一个 get_ops() 函数，分成单独头文件太碎
 *   - 切换后端时只需改 kmain 里一行调用，不需要加/删 #include
 *   - 新增后端只在此文件加一行声明即可
 */

#include "pmm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* bitmap 后端：简单位图分配器（每页 1 bit） */
const pmm_ops_t* pmm_bitmap_get_ops(void);

/* buddy 后端：Linux 风格伙伴系统（order 0..10） */
const pmm_ops_t* pmm_buddy_get_ops(void);

#ifdef __cplusplus
}
#endif
