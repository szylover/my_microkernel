#pragma once

/*
 * vmm.h — Virtual Memory Manager (VMM / Paging)
 *
 * Stage-3 goal (see docs/agent.md): enable x86 32-bit paging so every memory
 * access goes through the MMU (Memory Management Unit).
 *
 * ============================================================================
 * x86 32-bit 分页概览 (两级页表)
 * ============================================================================
 *
 * 虚拟地址 (32 bit) 被 CPU 拆成三段：
 *
 *   31        22 21       12 11        0
 *  +-----------+-----------+-----------+
 *  | PD index  | PT index  |  Offset   |
 *  |  (10 bit) |  (10 bit) |  (12 bit) |
 *  +-----------+-----------+-----------+
 *
 * - PD index (高 10 位): 在 Page Directory 中选一个 PDE (Page Directory Entry)
 * - PT index (中 10 位): 在 PDE 所指的 Page Table 中选一个 PTE (Page Table Entry)
 * - Offset   (低 12 位): 在 PTE 所指的 4KiB 页面内的偏移
 *
 * 地址翻译流程：
 *   CR3 → Page Directory[PD index] → Page Table[PT index] → 物理页 + Offset
 *
 * ============================================================================
 * 关键数据结构
 * ============================================================================
 *
 * Page Directory (PD):
 *   - 1024 个 PDE，每个 4 字节，共 4KiB（正好一页）
 *   - 物理地址加载到 CR3 寄存器
 *
 * Page Table (PT):
 *   - 1024 个 PTE，每个 4 字节，共 4KiB（正好一页）
 *   - 由 PDE 的高 20 位指向
 *
 * PDE / PTE 的低 12 位是标志位（flags），高 20 位是页帧地址：
 *
 *   31                12 11  9 8 7 6 5 4 3 2 1 0
 *  +--------------------+-----+-+-+-+-+-+-+-+-+-+
 *  | Page Frame Address | AVL |G|S|0|A|D|W|U|R|P|
 *  |     (20 bits)      |     | | | | | |T| |W| |
 *  +--------------------+-----+-+-+-+-+-+-+-+-+-+
 *
 * [BITFIELDS] 逐位说明：
 *   bit 0  P   (Present)       : 1=此条目有效，0=访问会触发 Page Fault (#PF)
 *   bit 1  R/W (Read/Write)    : 1=可读写，0=只读
 *   bit 2  U/S (User/Supervisor): 1=Ring 3 可访问，0=仅 Ring 0
 *   bit 3  PWT (Write-Through) : 1=写透缓存策略（通常置 0）
 *   bit 4  PCD (Cache Disable) : 1=禁用缓存（通常置 0）
 *   bit 5  A   (Accessed)      : CPU 访问过此页后自动置 1
 *   bit 6  D   (Dirty)         : CPU 写过此页后自动置 1 (仅 PTE 有意义)
 *   bit 7  PS/PAT              : PDE 中: PS=1 表示 4MiB 大页; PTE 中: PAT 位
 *   bit 8  G   (Global)        : 1=TLB 不随 CR3 切换而刷新（需 CR4.PGE）
 *   bits 9-11 AVL              : 软件可用位
 *   bits 12-31                 : 4KiB 物理页帧地址的高 20 位
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 常量定义
 * ============================================================================ */

#define VMM_PAGE_SIZE       4096u
#define VMM_ENTRIES_PER_PT  1024u   /* 每个页表有 1024 个 PTE */
#define VMM_ENTRIES_PER_PD  1024u   /* 页目录有 1024 个 PDE */

/*
 * High-Half Kernel 地址转换
 *
 * [WHY] 内核链接在虚拟地址 0xC0000000+ (high-half)，但物理内存从 0 开始。
 *   内核的直接映射区域: virt = phys + KERNEL_VIRT_OFFSET
 *   这两个宏用于在物理地址和虚拟地址之间转换。
 *
 * [CRITICAL] 仅适用于内核直接映射区域（0xC0000000 → 物理 0）。
 *   不适用于任意虚拟地址（需要查页表）。
 */
#define KERNEL_VIRT_OFFSET  0xC0000000u

#define PHYS_TO_VIRT(paddr) ((void*)((uint32_t)(paddr) + KERNEL_VIRT_OFFSET))
#define VIRT_TO_PHYS(vaddr) ((uint32_t)(vaddr) - KERNEL_VIRT_OFFSET)

/*
 * PDE / PTE flags (低 12 位)
 *
 * [BITFIELDS] 这些宏可以用 | 组合，例如：
 *   PTE_PRESENT | PTE_WRITABLE  => 页面存在 + 可写
 */
#define PTE_PRESENT     0x001u  /* bit 0: 条目有效 */
#define PTE_WRITABLE    0x002u  /* bit 1: 可读写   */
#define PTE_USER        0x004u  /* bit 2: 用户态可访问 */

/* PDE 专用 flags，值跟 PTE 一样（硬件格式相同） */
#define PDE_PRESENT     0x001u
#define PDE_WRITABLE    0x002u
#define PDE_USER        0x004u

/* ============================================================================
 * 类型定义
 * ============================================================================ */

/*
 * page_directory_t — 代表一个完整的页目录
 *
 * [WHY] 用 typedef 包装而非裸 uint32_t[1024]：
 *   方便日后给 page_directory 加元数据（如引用计数、所属进程等）。
 *
 * [CPU STATE]
 *   当此页目录被加载到 CR3 后，CPU 将以它为顶层来翻译所有虚拟地址。
 */
typedef struct {
    uint32_t entries[VMM_ENTRIES_PER_PD];
} __attribute__((aligned(VMM_PAGE_SIZE))) page_directory_t;

/*
 * page_table_t — 代表一个页表（被 PDE 指向）
 */
typedef struct {
    uint32_t entries[VMM_ENTRIES_PER_PT];
} __attribute__((aligned(VMM_PAGE_SIZE))) page_table_t;

/* ============================================================================
 * API
 * ============================================================================ */

/*
 * vmm_init — 初始化虚拟内存管理器并开启分页
 *
 * 做三件事：
 *   1. 分配并填充 Page Directory
 *   2. 建立 Identity Mapping（虚拟地址 == 物理地址，覆盖内核+已用内存）
 *   3. 将 PD 地址写入 CR3，然后置位 CR0.PG 开启分页
 *
 * [CPU STATE] 调用后：
 *   - CR3 指向新页目录
 *   - CR0.PG = 1，所有内存访问都经过 MMU
 *   - 内核代码继续运行是因为 identity mapping 保证 virt == phys
 *
 * 前置条件：PMM 已初始化（需要 pmm_alloc_page 来分配页表所需的物理页）。
 */
void vmm_init(void);

/*
 * vmm_unmap_identity — 拆除低地址 identity mapping
 *
 * 清除 PD[0..767]，使虚拟地址 0x00000000 - 0xBFFFFFFF 不再可访问。
 * 调用前必须确保所有指针已经通过 PHYS_TO_VIRT 转换为 high-half 地址。
 *
 * [CPU STATE] 调用后任何低地址访问将触发 #PF。
 */
void vmm_unmap_identity(void);

/*
 * vmm_map_page — 将一个虚拟页映射到一个物理页
 *
 * @param virt   虚拟地址（必须 4KiB 对齐）
 * @param phys   物理地址（必须 4KiB 对齐）
 * @param flags  PTE flags（PTE_PRESENT | PTE_WRITABLE 等）
 * @return       0 成功, -1 失败（PMM 无法分配页表）
 *
 * [WHY]
 *   将来内核需要按需映射新页（如用户态进程的地址空间、内存映射 I/O 等）。
 *   这个函数是最基本的"建立映射"原语。
 *
 * [CPU STATE]
 *   修改当前页目录中的 PDE/PTE；如果该虚拟地址已在 TLB 中缓存，
 *   需要调用 invlpg 刷新 TLB。
 */
int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);

/*
 * vmm_unmap_page — 解除一个虚拟页的映射
 *
 * @param virt   虚拟地址（必须 4KiB 对齐）
 *
 * [WHY]
 *   释放映射时需要清除 PTE 并刷新 TLB，但 **不会** 自动释放物理页
 *   （调用者负责决定是否 pmm_free_page）。
 */
void vmm_unmap_page(uint32_t virt);

/*
 * vmm_is_mapped — 查询一个虚拟地址是否已映射
 *
 * @param virt   虚拟地址（必须 4KiB 对齐）
 * @return       1 = 已映射 (Present), 0 = 未映射, -1 = VMM 未初始化
 */
int vmm_is_mapped(uint32_t virt);

/*
 * vmm_get_physical — 查询一个虚拟地址映射到的物理地址
 *
 * @param virt   虚拟地址（必须 4KiB 对齐）
 * @param phys   输出参数：物理地址（仅成功时有效）
 * @return       0 成功, -1 未映射或 VMM 未初始化
 */
int vmm_get_physical(uint32_t virt, uint32_t* phys);

/*
 * vmm_get_pde — 获取指定 PD index 的原始 PDE 值
 * vmm_get_pte — 获取指定虚拟地址对应的原始 PTE 值
 *
 * 直接返回硬件格式的 32 位值，高 20 位 = 页帧地址，低 12 位 = flags。
 * 返回 0 表示条目不存在或 VMM 未初始化。
 */
uint32_t vmm_get_pde(uint32_t pd_idx);
uint32_t vmm_get_pte(uint32_t virt);

/* 返回 VMM 是否已初始化并开启分页 */
int vmm_is_ready(void);

#ifdef __cplusplus
}
#endif
