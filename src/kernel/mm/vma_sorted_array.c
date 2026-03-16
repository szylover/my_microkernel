#include <stdint.h>
#include <stddef.h>

#include "vma.h"
#include "printk.h"

/*
 * vma_sorted_array.c — Sorted-Array VMA 后端（早期简单实现）
 *
 * ============================================================================
 * 核心思路
 * ============================================================================
 *
 * 用一个静态数组 g_areas[] 存储所有 VMA，按起始地址升序排列。
 * g_count 跟踪当前已注册的 VMA 数量。
 *
 *   g_areas (按 start 升序)
 *   ┌──────────┬──────────┬──────────┬─────────┬──────────┐
 *   │ area[0]  │ area[1]  │ area[2]  │  ...    │ area[N-1]│
 *   │ start=A  │ start=B  │ start=C  │         │ start=Z  │
 *   │ (A < B)  │ (B < C)  │          │         │          │
 *   └──────────┴──────────┴──────────┴─────────┴──────────┘
 *                               ▲
 *                          g_count = N
 *
 * [WHY] 为什么用排序数组而不是链表？
 *   1. find(addr) 可以用二分查找，O(log N)
 *   2. dump() 天然按地址排序，不需要额外排序
 *   3. 内核早期 VMA 数量很少（< 20 个），数组的 O(N) 插入/删除开销可忽略
 *   4. 实现简单，不需要 kmalloc（静态数组），适合内核启动阶段
 *
 * [WHY] 为什么不直接用链表？
 *   链表 find 是 O(N)，且缓存不友好（节点分散在内存中）。
 *   排序数组在 N < 64 时，即使插入 O(N)，总体性能远优于链表。
 *
 * 后续升级路径：
 *   当进程管理（Stage 13）引入 per-process VMA 后，VMA 数量可能爆炸增长，
 *   那时再切换到红黑树后端（vma_rbtree.c），接口完全兼容，零改动调用者代码。
 *
 * ============================================================================
 * 关键操作的实现策略
 * ============================================================================
 *
 * init:
 *   清零 g_areas[]，g_count = 0。
 *
 * add(start, end, flags, name):
 *   1. 检查参数合法性（页对齐、start < end、容量未满）
 *   2. 检查与已有 VMA 是否重叠（遍历数组）
 *   3. 找到插入位置（保持升序）
 *   4. 后移元素，腾出位置
 *   5. 写入新 VMA
 *
 *   插入示意（在 index=2 处插入新 VMA）：
 *
 *     插入前:  [A] [B] [D] [E] [_] [_]    g_count=4
 *                       ↑
 *                   需要把 D,E 后移一格
 *
 *     后移:    [A] [B] [_] [D] [E] [_]
 *                       ↑
 *                   空出 index=2
 *
 *     写入:    [A] [B] [C] [D] [E] [_]    g_count=5
 *
 * remove(start):
 *   1. 找到 start 匹配的 VMA（线性查找即可，N 很小）
 *   2. 前移后续元素覆盖被删除位置
 *   3. g_count--
 *
 *   删除示意（删除 index=2）：
 *
 *     删除前:  [A] [B] [C] [D] [E]    g_count=5
 *                       ↑
 *                   需要把 D,E 前移一格
 *
 *     前移:    [A] [B] [D] [E] [_]    g_count=4
 *
 * find(addr):
 *   二分查找：找最后一个 start <= addr 的 VMA，
 *   然后检查 addr < end。
 *
 *   [WHY] 为什么是"最后一个 start <= addr"？
 *     因为数组按 start 升序排列，且 VMA 不重叠，
 *     所以如果 addr 属于某个 VMA，那个 VMA 一定是
 *     start <= addr 的最大者。
 *
 *   二分查找示意：
 *
 *     g_areas:  [0x1000, 0x5000) [0x8000, 0xA000) [0xC000, 0xF000)
 *                  area[0]            area[1]            area[2]
 *
 *     find(0x9000):
 *       → 二分找到 area[1] (start=0x8000 <= 0x9000, 下一个 start=0xC000 > 0x9000)
 *       → 检查 0x9000 < 0xA000? 是 → 返回 area[1]
 *
 *     find(0xB000):
 *       → 二分找到 area[1] (start=0x8000 <= 0xB000, 下一个 start=0xC000 > 0xB000)
 *       → 检查 0xB000 < 0xA000? 否 → 返回 NULL（在两个 VMA 之间的空洞）
 *
 * dump:
 *   从头到尾遍历 g_areas[]，逐个打印。
 *
 * count:
 *   返回 g_count。
 */

/* ============================================================================
 * 容量常量
 *
 * [WHY] 256 个 VMA 条目：
 *   - 每个 vm_area_t = 16 字节，256 × 16 = 4096 字节 = 恰好 1 页
 *   - 足以管理 4GiB 地址空间（256 个不重叠区间，平均每个 16MiB）
 *   - 引导阶段 direct-map + kheap = 2 个，VMM 动态分配 ~20 个，
 *     进入多进程前不会用完
 *   - 若将来不够，切换红黑树后端即可
 * ============================================================================ */

#define VMA_MAX_AREAS 256

/* ============================================================================
 * 内部数据
 * ============================================================================ */

static vm_area_t g_areas[VMA_MAX_AREAS];
static unsigned  g_count = 0;

/* ============================================================================
 * 二分查找辅助函数
 *
 * [WHY] sa_add / sa_remove / sa_find 都需要在有序数组上做二分查找，
 *   区别仅在于比较条件（< vs <=）。抽成两个辅助函数避免重复代码：
 *
 *   lower_bound(target): 第一个 start >= target 的下标
 *     → sa_add 找插入位，sa_remove 找精确匹配
 *
 *   upper_bound(target): 第一个 start > target 的下标
 *     → sa_find 找"最后一个 start <= addr"（即 upper_bound - 1）
 *
 *   两者都返回 [0, g_count] 范围的下标（g_count 表示"所有都小于 target"）。
 * ============================================================================ */

static unsigned sa_lower_bound(uint32_t target)
{
    unsigned left = 0, right = g_count;
    while (left < right) {
        unsigned mid = left + (right - left) / 2;
        if (g_areas[mid].start < target)
            left = mid + 1;
        else
            right = mid;
    }
    return left;
}

static unsigned sa_upper_bound(uint32_t target)
{
    unsigned left = 0, right = g_count;
    while (left < right) {
        unsigned mid = left + (right - left) / 2;
        if (g_areas[mid].start <= target)
            left = mid + 1;
        else
            right = mid;
    }
    return left;
}

/* ============================================================================
 * sa_init — 初始化后端
 *
 * TODO(你来实现):
 *   清零 g_areas 数组，g_count 置 0。
 *   提示：可以用一个循环把每个 g_areas[i] 的字段清零，
 *        也可以写个小 memset 辅助函数。
 * ============================================================================ */

static void sa_init(void)
{
    /* --- 你的代码 --- */
    for (unsigned i = 0; i < VMA_MAX_AREAS; i++) {
        g_areas[i].start = 0;
        g_areas[i].end   = 0;
        g_areas[i].flags = 0;
        g_areas[i].name  = NULL;
    }

    g_count = 0;
}

/* ============================================================================
 * sa_add — 注册一个新的 VMA
 *
 * @param start  起始地址（页对齐，包含）
 * @param end    结束地址（页对齐，不包含），必须 > start
 * @param flags  权限标志（VMA_READ | VMA_WRITE | VMA_EXEC 的组合）
 * @param name   人类可读名称（指针直接存储，不复制字符串）
 * @return       0 成功, -1 失败
 *
 * TODO(你来实现), 分四步:
 *
 * 第 1 步：参数校验
 *   - g_count >= VMA_MAX_AREAS → 容量满，返回 -1
 *   - start >= end            → 无效范围，返回 -1
 *   - start 或 end 未页对齐   → 返回 -1
 *     页对齐检查：(addr & 0xFFF) == 0
 *     （0xFFF = 4096 - 1，即低 12 位全零则页对齐）
 *
 * 第 2 步：重叠检查
 *   遍历 g_areas[0..g_count)，检查新 [start, end) 是否和已有 VMA 重叠。
 *   两个区间 [a_start, a_end) 和 [b_start, b_end) 重叠的条件是：
 *     a_start < b_end && b_start < a_end
 *   （画个数轴就能理解：两个区间"不重叠"意味着一个完全在另一个左边）
 *   如果有重叠，返回 -1。
 *
 * 第 3 步：找插入位置
 *   因为数组按 start 升序排列，找到第一个 g_areas[i].start > start 的位置。
 *   所有 i 之后的元素需要后移一格。
 *
 *   提示：用一个变量 pos 从 0 开始，当 pos < g_count 且
 *         g_areas[pos].start < start 时，pos++。
 *         循环结束后 pos 就是插入位置。
 *
 * 第 4 步：后移 + 写入
 *   - 从尾部 (g_count-1) 到 pos，逐个往后搬：
 *       for (i = g_count; i > pos; i--)
 *           g_areas[i] = g_areas[i-1];
 *   - 在 g_areas[pos] 写入新 VMA 的四个字段
 *   - g_count++
 *   - 返回 0
 * ============================================================================ */

static int sa_add(uint32_t start, uint32_t end, uint32_t flags, const char* name)
{
    /* --- 你的代码 --- */
    if (g_count >= VMA_MAX_AREAS) {
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

    for (unsigned i = 0; i < g_count; i++) {
        if (start < g_areas[i].end && g_areas[i].start < end) {
            printk("[vma] ERROR: VMA [0x%08x, 0x%08x) '%s' overlaps with existing [0x%08x, 0x%08x) '%s'\n",
                   start, end, name ? name : "?",
                   g_areas[i].start, g_areas[i].end, g_areas[i].name ? g_areas[i].name : "?");
            return -1;
        }
    }

    unsigned pos = sa_lower_bound(start);

    for (unsigned i = g_count; i > pos; i--) {
        g_areas[i] = g_areas[i - 1];
    }

    g_areas[pos].start = start;
    g_areas[pos].end   = end;
    g_areas[pos].flags = flags;
    g_areas[pos].name  = name;
    g_count++;

    return 0;
}

/* ============================================================================
 * sa_remove — 按起始地址删除一个 VMA
 *
 * @param start  要删除的 VMA 的起始地址
 * @return       0 成功, -1 未找到
 *
 * TODO(你来实现), 分两步:
 *
 * 第 1 步：查找
 *   遍历 g_areas[0..g_count)，找到 g_areas[i].start == start 的条目。
 *   如果没找到，返回 -1。
 *
 * 第 2 步：前移 + 缩减
 *   把 i 之后的元素逐个前移：
 *     for (j = i; j < g_count - 1; j++)
 *         g_areas[j] = g_areas[j+1];
 *   g_count--
 *   返回 0
 * ============================================================================ */

static int sa_remove(uint32_t start)
{
    /* --- 你的代码 --- */
    unsigned pos = sa_lower_bound(start);

    if (pos == g_count || g_areas[pos].start != start) {
        printk("[vma] ERROR: cannot remove VMA with start=0x%08x: not found\n", start);
        return -1;
    }

    for (unsigned i = pos; i < g_count - 1; i++) {
        g_areas[i] = g_areas[i + 1];
    }
    g_count--;
    return 0;
}

/* ============================================================================
 * sa_find — 查找包含 addr 的 VMA
 *
 * @param addr   要查找的虚拟地址
 * @return       指向匹配的 vm_area_t，未找到返回 NULL
 *
 * TODO(你来实现):
 *
 * 用二分查找找到最后一个 start <= addr 的 VMA。
 *
 * 二分查找算法（标准 upper_bound 变体）：
 *
 *   lo = 0, hi = g_count
 *
 *   while (lo < hi):
 *       mid = lo + (hi - lo) / 2
 *
 *       if g_areas[mid].start <= addr:
 *           lo = mid + 1        ← mid 可能是答案，但看看右边有没有更大的
 *       else:
 *           hi = mid            ← mid 太大了，答案在左边
 *
 *   循环结束后，lo == hi，且 lo - 1 是最后一个 start <= addr 的下标。
 *
 *   [WHY] 为什么用 upper_bound 而不是普通 lower_bound？
 *     因为我们要的是"最后一个 start <= addr"，即 start 恰好 <= addr 的
 *     那个 VMA。upper_bound 找的是第一个 start > addr 的位置，减 1 就是答案。
 *
 *   检查：
 *     - 如果 lo == 0，说明所有 VMA 的 start 都 > addr → 返回 NULL
 *     - 否则候选者是 g_areas[lo - 1]
 *     - 检查 addr < 候选者.end？是则返回它，否则返回 NULL
 *       （addr 落在两个 VMA 之间的空洞里）
 * ============================================================================ */

static const vm_area_t* sa_find(uint32_t addr)
{
    if (g_count == 0)
        return NULL;

    unsigned idx = sa_upper_bound(addr);
    if (idx == 0)
        return NULL;

    const vm_area_t* candidate = &g_areas[idx - 1];
    return (addr < candidate->end) ? candidate : NULL;
}

/* ============================================================================
 * sa_dump — 打印所有 VMA（数组已按地址排序，直接遍历即可）
 *
 * TODO(你来实现):
 *
 * 打印格式（和 cmd_vma.c 中 find 子命令保持一致）：
 *   printk("[%2u] 0x%08x - 0x%08x  %4u KiB  %c%c%c  %s\n",
 *          i, area.start, area.end,
 *          (area.end - area.start) / 1024,
 *          area.flags & VMA_READ  ? 'r' : '-',
 *          area.flags & VMA_WRITE ? 'w' : '-',
 *          area.flags & VMA_EXEC  ? 'x' : '-',
 *          area.name ? area.name : "?");
 *
 * 遍历 g_areas[0..g_count)，逐个打印。
 * 如果 g_count == 0，打印 "(no VMAs registered)"。
 * ============================================================================ */

static void sa_dump(void)
{
    if (g_count == 0) {
        printk("(no VMAs registered)\n");
        return;
    }

    for (unsigned i = 0; i < g_count; i++) {
        const vm_area_t* area = &g_areas[i];
        printk("[%2u] 0x%08x - 0x%08x  %4u KiB  %c%c%c  %s\n",
               i, area->start, area->end,
               (area->end - area->start) / 1024,
               area->flags & VMA_READ  ? 'r' : '-',
               area->flags & VMA_WRITE ? 'w' : '-',
               area->flags & VMA_EXEC  ? 'x' : '-',
               area->name ? area->name : "?");
    }
}

/* ============================================================================
 * sa_count — 返回当前 VMA 数量
 * ============================================================================ */

static unsigned sa_count(void)
{
    return g_count;
}

/* ============================================================================
 * 后端操作表（导出给 vma.c dispatch 层）
 *
 * [WHY] 和 pmm_bitmap.c / heap_first_fit.c 同一套模式：
 *   用 static const 结构体把所有函数指针打包，
 *   通过 vma_sorted_array_get_ops() 返回给上层。
 * ============================================================================ */

static const vma_ops_t sa_ops = {
    .name   = "sorted-array",
    .init   = sa_init,
    .add    = sa_add,
    .remove = sa_remove,
    .find   = sa_find,
    .dump   = sa_dump,
    .count  = sa_count,
};

const vma_ops_t* vma_sorted_array_get_ops(void)
{
    return &sa_ops;
}
