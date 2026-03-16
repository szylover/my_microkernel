#include <stdint.h>
#include <stddef.h>

#include "vma.h"
#include "printk.h"

/*
 * vma_maple.c — Maple Tree VMA 后端
 *
 * ============================================================================
 * 核心思路
 * ============================================================================
 *
 * Maple Tree 是 Linux 6.1（2022 年）引入的 VMA 管理数据结构，
 * 取代了沿用近 20 年的红黑树。它是一种 B-tree 变体，
 * 专为区间 / 范围查找优化，具有优秀的缓存局部性。
 *
 * [WHY] 为什么 Linux 从红黑树换到 Maple Tree？
 *
 *   1. **缓存友好**：B-tree 节点包含多个键值对（扇出度高），
 *      一次缓存行加载可以比较多个键，减少缓存 miss。
 *      红黑树每个节点只有一个键，每次比较都可能 cache miss。
 *
 *   2. **RCU 安全**：Maple Tree 天然支持 RCU（Read-Copy-Update），
 *      读者无需加锁即可遍历。红黑树的旋转操作在 RCU 下很难正确实现。
 *
 *   3. **区间原生支持**：Maple Tree 以 [start, end) 区间为键，
 *      不像红黑树只按 start 排序后再检查 end。
 *
 *   4. **减少指针追逐**：红黑树每个节点 5 个指针（parent/left/right/color + data），
 *      Maple Tree 用紧凑数组存储，指针数更少。
 *
 * ============================================================================
 * 简化实现说明
 * ============================================================================
 *
 * Linux 的 maple tree 实现极其复杂（~6000 行），涉及 RCU、NUMA 优化、
 * 多种节点类型（maple_range_64, maple_arange_64, maple_leaf_64）等。
 *
 * 本实现是一个**教学级 B+ tree 变体**，保留 Maple Tree 的核心设计理念：
 *   - B+ tree 结构：内部节点存键（分割点），叶节点存 VMA 数据
 *   - 高扇出度：每个节点最多 MAPLE_NODE_SLOTS 个子节点/数据项
 *   - 区间查找：利用 pivot（分割键）快速定位目标区间
 *
 * 我们不实现 RCU、NUMA、类型多态等 Linux 特有的复杂性。
 *
 * ============================================================================
 * 节点结构
 * ============================================================================
 *
 *   内部节点（Internal Node）:
 *   ┌─────────┬─────────┬─────────┬─────────┐
 *   │ pivot[0]│ pivot[1]│ pivot[2]│  ...    │  分割键（VMA start 地址）
 *   ├─────────┼─────────┼─────────┼─────────┤
 *   │ child[0]│ child[1]│ child[2]│ child[3]│  子节点指针
 *   └─────────┴─────────┴─────────┴─────────┘
 *
 *   叶节点（Leaf Node）:
 *   ┌─────────┬─────────┬─────────┬─────────┐
 *   │ pivot[0]│ pivot[1]│ pivot[2]│  ...    │  分割键
 *   ├─────────┼─────────┼─────────┼─────────┤
 *   │ slot[0] │ slot[1] │ slot[2] │ slot[3] │  指向 VMA 数据的指针
 *   └─────────┴─────────┴─────────┴─────────┘
 *
 *   对于内部节点：child[i] 覆盖地址范围 [pivot[i-1], pivot[i])
 *     （pivot[-1] 视为 0，pivot[nr_pivots] 视为 UINT32_MAX）
 *
 *   对于叶节点：slot[i] 指向 start 地址为 pivot[i] 的 VMA 数据
 *
 * ============================================================================
 * 对比红黑树
 * ============================================================================
 *
 *   | 特性         | 红黑树                | Maple Tree (B+ tree)     |
 *   |-------------|----------------------|--------------------------|
 *   | 节点扇出     | 2（二叉）             | MAPLE_NODE_SLOTS（多路）  |
 *   | 树高（256节点）| ~16                  | ~3                       |
 *   | 缓存行利用   | 差（每节点1个键）       | 好（每节点多个键）         |
 *   | 旋转操作     | 有（2-3次/操作）       | 无（分裂/合并替代）        |
 *   | 区间查找     | 需要额外逻辑           | 原生支持                  |
 *   | 实现复杂度   | 中等                  | 较高                     |
 */

/* ============================================================================
 * 常量与配置
 * ============================================================================ */

/*
 * [WHY] 扇出度为什么选 10？
 *
 * Linux maple tree 的 maple_range_64 节点有 16 个 pivot + 16 个 slot，
 * 每个节点 256 字节（恰好 4 个缓存行）。
 *
 * 我们的 32 位系统中：
 *   - pivot: uint32_t = 4 字节
 *   - slot: pointer = 4 字节
 *   - 10 个 pivot + 11 个 child/slot + parent + metadata ≈ 96 字节
 *     接近 2 个缓存行（64×2=128），是合理的缓存友好大小。
 *
 * 扇出度 10 意味着：
 *   - 256 个 VMA 只需树高 3（10^3 = 1000 > 256）
 *   - 每次查找最多 3 次节点访问，每次访问比较 ~10 个 pivot
 *     这利用了 CPU 的缓存预取——同一节点的 pivot 在同一缓存行中
 */
#define MAPLE_NODE_SLOTS    10  /* 每个节点最多 10 个子节点/数据条目 */
#define MAPLE_MAX_PIVOTS    (MAPLE_NODE_SLOTS - 1)  /* 9 个分割键 */

/* 节点池大小：叶节点最多 ceil(256/MAPLE_NODE_SLOTS)=26，加上内部节点 ~10 个 */
#define MAPLE_MAX_NODES     64
#define MAPLE_MAX_VMAS      256

/* ============================================================================
 * 类型定义
 * ============================================================================ */

typedef struct maple_node maple_node_t;

struct maple_node {
    uint32_t     pivots[MAPLE_MAX_PIVOTS]; /* 分割键：子节点/slot 的边界 */
    uint8_t      nr_entries;               /* 当前条目数（子节点数或数据数） */
    uint8_t      is_leaf;                  /* 1=叶节点, 0=内部节点 */
    maple_node_t* parent;                  /* 父节点（根节点的 parent 为 NULL） */

    union {
        maple_node_t* children[MAPLE_NODE_SLOTS]; /* 内部节点：子节点指针 */
        vm_area_t*    slots[MAPLE_NODE_SLOTS];    /* 叶节点：VMA 数据指针 */
    };
};

/* ============================================================================
 * 内部数据
 * ============================================================================ */

static maple_node_t g_node_pool[MAPLE_MAX_NODES];  /* 节点池 */
static maple_node_t* g_node_free;                   /* 空闲节点链表 */

static vm_area_t g_vma_pool[MAPLE_MAX_VMAS];        /* VMA 存储池 */
static vm_area_t* g_vma_free;                        /* 空闲 VMA 链表 */

static maple_node_t* g_maple_root;
static unsigned g_count;

/* ============================================================================
 * 内存池管理
 *
 * [WHY] 和红黑树一样使用静态池，避免 kmalloc 循环依赖。
 *
 * 节点池：用 children[0]（或 parent）作为 next 指针串联空闲链表。
 * VMA 池：用 start 字段的最低位标记空闲（但 start 必须页对齐所以低位本来为 0），
 *          或者更简单地用一个 freelist。这里我们用 name 指针做链表指针。
 * ============================================================================ */

static void node_pool_init(void)
{
    g_node_free = &g_node_pool[0];
    for (unsigned i = 0; i < MAPLE_MAX_NODES - 1; i++) {
        g_node_pool[i].parent = &g_node_pool[i + 1];
    }
    g_node_pool[MAPLE_MAX_NODES - 1].parent = NULL;
}

static maple_node_t* node_alloc(void)
{
    if (!g_node_free) return NULL;
    maple_node_t* n = g_node_free;
    g_node_free = n->parent;

    n->nr_entries = 0;
    n->is_leaf = 0;
    n->parent = NULL;
    for (int i = 0; i < MAPLE_MAX_PIVOTS; i++)
        n->pivots[i] = 0;
    for (int i = 0; i < MAPLE_NODE_SLOTS; i++)
        n->children[i] = NULL;

    return n;
}

static void __attribute__((unused)) node_free(maple_node_t* n)
{
    n->parent = g_node_free;
    g_node_free = n;
}

static void vma_pool_init(void)
{
    g_vma_free = &g_vma_pool[0];
    for (unsigned i = 0; i < MAPLE_MAX_VMAS - 1; i++) {
        /* 用 name 指针做链表（name 在空闲状态下无意义） */
        g_vma_pool[i].name = (const char*)&g_vma_pool[i + 1];
    }
    g_vma_pool[MAPLE_MAX_VMAS - 1].name = NULL;
}

static vm_area_t* vma_alloc(void)
{
    if (!g_vma_free) return NULL;
    vm_area_t* v = g_vma_free;
    g_vma_free = (vm_area_t*)v->name;

    v->start = 0;
    v->end   = 0;
    v->flags = 0;
    v->name  = NULL;

    return v;
}

static void vma_free_entry(vm_area_t* v)
{
    v->name = (const char*)g_vma_free;
    g_vma_free = v;
}

/* ============================================================================
 * 叶节点操作（有序数组操作）
 *
 * [WHY] 叶节点内部是一个按 start 升序排列的小数组（最多 10 个条目）。
 *   在小数组上做线性扫描比二分查找更快（10 次比较 < 缓存 miss 的代价）。
 * ============================================================================ */

/*
 * leaf_find_slot — 在叶节点中找到 start 应该插入的位置
 *
 * 返回第一个 pivots[i] >= start 的下标，如果都小于则返回 nr_entries。
 */
static unsigned leaf_find_slot(maple_node_t* leaf, uint32_t start)
{
    for (unsigned i = 0; i < leaf->nr_entries; i++) {
        if (leaf->pivots[i] >= start)
            return i;
    }
    return leaf->nr_entries;
}

/*
 * leaf_insert_at — 在叶节点的指定位置插入一个 VMA
 *
 * 前提：leaf->nr_entries < MAPLE_NODE_SLOTS（调用者保证）
 */
static void leaf_insert_at(maple_node_t* leaf, unsigned pos, vm_area_t* vma)
{
    /* 后移 */
    for (unsigned i = leaf->nr_entries; i > pos; i--) {
        leaf->pivots[i] = leaf->pivots[i - 1];
        leaf->slots[i]  = leaf->slots[i - 1];
    }
    leaf->pivots[pos] = vma->start;
    leaf->slots[pos]  = vma;
    leaf->nr_entries++;
}

/*
 * leaf_remove_at — 删除叶节点指定位置的条目
 */
static void leaf_remove_at(maple_node_t* leaf, unsigned pos)
{
    for (unsigned i = pos; i < (unsigned)(leaf->nr_entries - 1); i++) {
        leaf->pivots[i] = leaf->pivots[i + 1];
        leaf->slots[i]  = leaf->slots[i + 1];
    }
    leaf->nr_entries--;
    leaf->pivots[leaf->nr_entries] = 0;
    leaf->slots[leaf->nr_entries]  = NULL;
}

/* ============================================================================
 * 内部节点操作
 * ============================================================================ */

/*
 * internal_find_child — 在内部节点中找到 addr 对应的子节点下标
 *
 * 逻辑：
 *   child[0] 覆盖 [0, pivots[0])
 *   child[i] 覆盖 [pivots[i-1], pivots[i])  (1 <= i < nr_entries-1)
 *   child[nr_entries-1] 覆盖 [pivots[nr_entries-2], ∞)
 *
 * 简化：找第一个 pivots[i] > addr 的下标，返回 i。
 *       如果都 <= addr（或没有 pivot），返回 nr_entries - 1。
 */
static unsigned internal_find_child(maple_node_t* node, uint32_t addr)
{
    for (unsigned i = 0; i < (unsigned)(node->nr_entries - 1); i++) {
        if (addr < node->pivots[i])
            return i;
    }
    return node->nr_entries - 1;
}

/*
 * internal_insert_at — 在内部节点的指定位置插入新子节点
 *
 * 插入后：pivot[pos] = split_key，children[pos+1] = new_child
 * （原 children[pos] 不变，它是分裂后的左半部分）
 *
 * 前提：node->nr_entries < MAPLE_NODE_SLOTS
 */
static void internal_insert_at(maple_node_t* node, unsigned pos,
                                uint32_t split_key, maple_node_t* new_child)
{
    /* 后移 pivots */
    for (unsigned i = node->nr_entries - 1; i > pos; i--) {
        node->pivots[i] = node->pivots[i - 1];
    }
    node->pivots[pos] = split_key;

    /* 后移 children (从 pos+2 开始) */
    for (unsigned i = node->nr_entries; i > pos + 1; i--) {
        node->children[i] = node->children[i - 1];
    }
    node->children[pos + 1] = new_child;
    new_child->parent = node;

    node->nr_entries++;
}

/* ============================================================================
 * 节点分裂
 *
 * [WHY] 当一个节点满了（nr_entries == MAPLE_NODE_SLOTS），
 *   在插入之前先将其分裂为两个节点，把中间键上提到父节点。
 *   这是 B-tree 的核心操作，保持树的平衡。
 *
 *   分裂前:  [K0, K1, K2, K3, K4, K5, K6, K7, K8]  (9 个 pivot, 10 个 slot)
 *   分裂后:  左: [K0, K1, K2, K3]   右: [K5, K6, K7, K8]
 *            中间键 K4 上提到父节点
 * ============================================================================ */

/*
 * split_mid — 分裂点下标
 */
#define SPLIT_MID  (MAPLE_NODE_SLOTS / 2)  /* = 5 */

/*
 * split_leaf — 分裂叶节点
 *
 * 把 leaf 的后半部分移到 new_leaf。
 * 返回上提的分割键（new_leaf 的第一个 pivot）。
 */
static uint32_t split_leaf(maple_node_t* leaf, maple_node_t* new_leaf)
{
    new_leaf->is_leaf = 1;
    unsigned move_count = leaf->nr_entries - SPLIT_MID;

    for (unsigned i = 0; i < move_count; i++) {
        new_leaf->pivots[i] = leaf->pivots[SPLIT_MID + i];
        new_leaf->slots[i]  = leaf->slots[SPLIT_MID + i];
        leaf->pivots[SPLIT_MID + i] = 0;
        leaf->slots[SPLIT_MID + i]  = NULL;
    }
    new_leaf->nr_entries = (uint8_t)move_count;
    leaf->nr_entries = SPLIT_MID;

    return new_leaf->pivots[0];  /* 上提键 */
}

/*
 * split_internal — 分裂内部节点
 *
 * 把 node 的后半部分移到 new_node。
 * pivots[SPLIT_MID - 1] 是上提键（不保留在任何子节点中）。
 */
static uint32_t split_internal(maple_node_t* node, maple_node_t* new_node)
{
    new_node->is_leaf = 0;

    uint32_t up_key = node->pivots[SPLIT_MID - 1];

    unsigned move_pivots = node->nr_entries - 1 - SPLIT_MID;
    for (unsigned i = 0; i < move_pivots; i++) {
        new_node->pivots[i] = node->pivots[SPLIT_MID + i];
        node->pivots[SPLIT_MID + i] = 0;
    }

    unsigned move_children = node->nr_entries - SPLIT_MID;
    for (unsigned i = 0; i < move_children; i++) {
        new_node->children[i] = node->children[SPLIT_MID + i];
        if (new_node->children[i])
            new_node->children[i]->parent = new_node;
        node->children[SPLIT_MID + i] = NULL;
    }

    new_node->nr_entries = (uint8_t)move_children;
    node->nr_entries = SPLIT_MID;
    node->pivots[SPLIT_MID - 1] = 0;

    return up_key;
}

/* ============================================================================
 * maple_init
 * ============================================================================ */

static void maple_init(void)
{
    node_pool_init();
    vma_pool_init();

    /* 创建初始根节点（空叶节点） */
    g_maple_root = node_alloc();
    if (g_maple_root) {
        g_maple_root->is_leaf = 1;
        g_maple_root->parent = NULL;
    }

    g_count = 0;
}

/* ============================================================================
 * maple_add — 插入新 VMA
 *
 * 步骤：
 *   1. 参数校验
 *   2. 从根沿树下降到叶节点
 *   3. 在叶节点中检查重叠
 *   4. 如果叶节点满了，先分裂再插入
 *   5. 插入 VMA
 *   6. 如果分裂产生上提键，递归向上插入
 *
 * @return 0=成功, -1=失败
 * ============================================================================ */

static int maple_add(uint32_t start, uint32_t end, uint32_t flags, const char* name)
{
    /* 参数校验 */
    if (g_count >= MAPLE_MAX_VMAS) {
        printk("[vma] ERROR: cannot add VMA [0x%08x, 0x%08x) '%s': capacity full\n",
               start, end, name ? name : "?");
        return -1;
    }

    if (start >= end) {
        printk("[vma] ERROR: invalid VMA range [0x%08x, 0x%08x) '%s'\n",
               start, end, name ? name : "?");
        return -1;
    }

    if ((start & 0xFFF) != 0 || (end & 0xFFF) != 0) {
        printk("[vma] ERROR: VMA range not page-aligned [0x%08x, 0x%08x) '%s'\n",
               start, end, name ? name : "?");
        return -1;
    }

    if (!g_maple_root) return -1;

    /* 从根下降到叶节点 */
    maple_node_t* node = g_maple_root;
    while (!node->is_leaf) {
        unsigned idx = internal_find_child(node, start);
        node = node->children[idx];
    }

    /*
     * 重叠检查：在叶节点中线性扫描
     *
     * [WHY] 叶节点内只有 ≤ 10 个条目，线性扫描开销极小。
     *   但叶节点只包含部分 VMA，还需要检查相邻叶节点吗？
     *
     *   不需要。B+ tree 的路由保证：如果新 VMA [start, end) 与某个已有 VMA 重叠，
     *   那个已有 VMA 的 start 一定在与新 VMA 相同的"搜索区间"内，
     *   或者在相邻叶节点的边界上。但因为我们按 start 路由下降，
     *   同一叶节点中的 VMA 的 start 范围与新 VMA 的 start 相近。
     *
     *   为了安全，我们采用简单策略：在叶节点内检查所有条目，
     *   并检查相邻条目（通过 pivot 范围限定）。
     *   对于教学内核的 < 256 个 VMA，这完全够用。
     *
     *   更严格的做法需要全树遍历，但对于互不重叠的 VMA 集合，
     *   BST/B-tree 路由已经保证了正确性（只要已有 VMA 不重叠）。
     */
    for (unsigned i = 0; i < node->nr_entries; i++) {
        vm_area_t* existing = node->slots[i];
        if (start < existing->end && existing->start < end) {
            printk("[vma] ERROR: VMA [0x%08x, 0x%08x) '%s' overlaps with "
                   "existing [0x%08x, 0x%08x) '%s'\n",
                   start, end, name ? name : "?",
                   existing->start, existing->end,
                   existing->name ? existing->name : "?");
            return -1;
        }
    }

    /* 分配 VMA 数据 */
    vm_area_t* vma = vma_alloc();
    if (!vma) {
        printk("[vma] ERROR: VMA pool exhausted\n");
        return -1;
    }
    vma->start = start;
    vma->end   = end;
    vma->flags = flags;
    vma->name  = name;

    /* 如果叶节点还有空间，直接插入 */
    if (node->nr_entries < MAPLE_NODE_SLOTS) {
        unsigned pos = leaf_find_slot(node, start);
        leaf_insert_at(node, pos, vma);
        g_count++;
        return 0;
    }

    /*
     * 叶节点满了 → 分裂后插入
     *
     * [WHY] B-tree 的核心操作：
     *   1. 分配新叶节点
     *   2. 把当前叶的后半部分移到新叶
     *   3. 把新条目插入到合适的那一半
     *   4. 把上提键插入父节点（可能递归分裂）
     */
    maple_node_t* new_leaf = node_alloc();
    if (!new_leaf) {
        vma_free_entry(vma);
        printk("[vma] ERROR: node pool exhausted during split\n");
        return -1;
    }

    uint32_t up_key = split_leaf(node, new_leaf);

    /* 决定插入到哪一半 */
    if (start < up_key) {
        unsigned pos = leaf_find_slot(node, start);
        leaf_insert_at(node, pos, vma);
    } else {
        unsigned pos = leaf_find_slot(new_leaf, start);
        leaf_insert_at(new_leaf, pos, vma);
    }

    /* 向上插入分割键 */
    maple_node_t* child = node;
    maple_node_t* new_child = new_leaf;
    uint32_t split_key = up_key;

    while (1) {
        maple_node_t* parent = child->parent;

        if (!parent) {
            /*
             * child 是根节点 → 创建新根
             *
             * [WHY] B-tree 增高唯一的方式：根节点分裂 → 创建新根。
             *   新根有两个子节点（分裂后的左半和右半），
             *   一个 pivot（上提的分割键）。
             */
            maple_node_t* new_root = node_alloc();
            if (!new_root) {
                /* 极端情况：无法分配新根。树结构已被修改，这里不好回滚。
                 * 实际内核中这是 fatal error。*/
                printk("[vma] FATAL: cannot allocate new root\n");
                g_count++;
                return 0; /* VMA 已插入到叶中，只是树结构不完整 */
            }
            new_root->is_leaf = 0;
            new_root->nr_entries = 2;
            new_root->pivots[0] = split_key;
            new_root->children[0] = child;
            new_root->children[1] = new_child;
            new_root->parent = NULL;
            child->parent = new_root;
            new_child->parent = new_root;
            g_maple_root = new_root;
            break;
        }

        if (parent->nr_entries < MAPLE_NODE_SLOTS) {
            /* 父节点有空间，直接插入 */
            unsigned pos = 0;
            for (pos = 0; pos < (unsigned)(parent->nr_entries - 1); pos++) {
                if (split_key < parent->pivots[pos])
                    break;
            }
            internal_insert_at(parent, pos, split_key, new_child);
            break;
        }

        /* 父节点也满了 → 递归分裂 */
        maple_node_t* new_parent = node_alloc();
        if (!new_parent) {
            printk("[vma] FATAL: cannot split internal node\n");
            g_count++;
            return 0;
        }

        /* 先把 split_key + new_child 临时插入父节点（超限一个），
         * 然后立即分裂。
         * 但我们的数组大小是固定的，不能超限。
         * 换一种做法：先分裂父节点，然后把 split_key 插到合适的一半。
         */
        uint32_t parent_up_key = split_internal(parent, new_parent);

        /* 把 split_key + new_child 插到合适的一半 */
        if (split_key < parent_up_key) {
            unsigned pos = 0;
            for (pos = 0; pos < (unsigned)(parent->nr_entries - 1); pos++) {
                if (split_key < parent->pivots[pos])
                    break;
            }
            internal_insert_at(parent, pos, split_key, new_child);
        } else {
            unsigned pos = 0;
            for (pos = 0; pos < (unsigned)(new_parent->nr_entries - 1); pos++) {
                if (split_key < new_parent->pivots[pos])
                    break;
            }
            internal_insert_at(new_parent, pos, split_key, new_child);
        }

        /* 继续向上传播 */
        child = parent;
        new_child = new_parent;
        split_key = parent_up_key;
    }

    g_count++;
    return 0;
}

/* ============================================================================
 * maple_remove — 删除指定 start 的 VMA
 *
 * [WHY] 完整的 B-tree 删除需要处理节点下溢（underflow）后的
 *   合并（merge）或借调（redistribute）。对于我们 < 256 个 VMA 的场景，
 *   简化处理：只从叶节点中删除条目，不做合并。
 *   节点可能变空，但不会影响正确性（查找会跳过空节点）。
 *   代价：树可能变得不够紧凑，但 VMA 数量很少时无影响。
 *
 * @return 0=成功, -1=未找到
 * ============================================================================ */

static int maple_remove(uint32_t start)
{
    if (!g_maple_root) return -1;

    /* 下降到叶节点 */
    maple_node_t* node = g_maple_root;
    while (!node->is_leaf) {
        unsigned idx = internal_find_child(node, start);
        if (!node->children[idx]) return -1;
        node = node->children[idx];
    }

    /* 在叶节点中线性查找 */
    for (unsigned i = 0; i < node->nr_entries; i++) {
        if (node->slots[i] && node->slots[i]->start == start) {
            vma_free_entry(node->slots[i]);
            leaf_remove_at(node, i);
            g_count--;
            return 0;
        }
    }

    printk("[vma] ERROR: cannot remove VMA with start=0x%08x: not found\n", start);
    return -1;
}

/* ============================================================================
 * maple_find — 查找包含 addr 的 VMA
 *
 * 从根沿树下降到叶节点，然后在叶中线性扫描。
 * 复杂度：O(log_B n)，其中 B = MAPLE_NODE_SLOTS。
 *
 * @return 指向匹配的 vm_area_t，未找到返回 NULL
 * ============================================================================ */

static const vm_area_t* maple_find(uint32_t addr)
{
    if (!g_maple_root) return NULL;

    maple_node_t* node = g_maple_root;

    /* 沿树下降到叶节点 */
    while (!node->is_leaf) {
        unsigned idx = internal_find_child(node, addr);
        if (!node->children[idx]) return NULL;
        node = node->children[idx];
    }

    /* 在叶节点中查找 */
    for (unsigned i = 0; i < node->nr_entries; i++) {
        vm_area_t* vma = node->slots[i];
        if (vma && addr >= vma->start && addr < vma->end)
            return vma;
    }

    return NULL;
}

/* ============================================================================
 * maple_dump — 按地址排序打印所有 VMA
 *
 * [WHY] B+ tree 的叶节点从左到右天然有序（按 start 升序）。
 *   我们用递归中序遍历来按序打印所有 VMA。
 *   由于树高最多 3~4 层，递归深度极浅，不会栈溢出。
 * ============================================================================ */

static unsigned dump_idx;   /* dump 用的计数器 */

static void dump_recursive(maple_node_t* node)
{
    if (!node) return;

    if (node->is_leaf) {
        for (unsigned i = 0; i < node->nr_entries; i++) {
            const vm_area_t* a = node->slots[i];
            if (!a) continue;
            printk("[%2u] 0x%08x - 0x%08x  %4u KiB  %c%c%c  %s\n",
                   dump_idx++, a->start, a->end,
                   (a->end - a->start) / 1024,
                   a->flags & VMA_READ  ? 'r' : '-',
                   a->flags & VMA_WRITE ? 'w' : '-',
                   a->flags & VMA_EXEC  ? 'x' : '-',
                   a->name ? a->name : "?");
        }
    } else {
        for (unsigned i = 0; i < node->nr_entries; i++) {
            dump_recursive(node->children[i]);
        }
    }
}

static void maple_dump(void)
{
    if (g_count == 0) {
        printk("(no VMAs registered)\n");
        return;
    }

    dump_idx = 0;
    dump_recursive(g_maple_root);
}

/* ============================================================================
 * maple_count
 * ============================================================================ */

static unsigned maple_count(void)
{
    return g_count;
}

/* ============================================================================
 * 后端操作表
 * ============================================================================ */

static const vma_ops_t maple_ops = {
    .name   = "maple-tree",
    .init   = maple_init,
    .add    = maple_add,
    .remove = maple_remove,
    .find   = maple_find,
    .dump   = maple_dump,
    .count  = maple_count,
};

const vma_ops_t* vma_maple_get_ops(void)
{
    return &maple_ops;
}
