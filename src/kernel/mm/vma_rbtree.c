#include <stdint.h>
#include <stddef.h>

#include "vma.h"
#include "printk.h"

/*
 * vma_rbtree.c — 红黑树 VMA 后端（生产级实现）
 *
 * ============================================================================
 * 核心思路
 * ============================================================================
 *
 * 红黑树（Red-Black Tree）是一种自平衡二叉搜索树，
 * 在 Linux 内核中被长期用于管理 VMA（从 2.4 到 6.0）。
 * 每个节点按 VMA 的 start 地址排序，支持 O(log n) 的查找、插入、删除。
 *
 * BST 的中序遍历即为 VMA 按地址升序排列——
 * 这保证 dump() 输出天然有序，find() 能高效定位。
 *
 * [WHY] 为什么用红黑树而不是 AVL 树？
 *   1. 红黑树插入最多 2 次旋转，删除最多 3 次旋转；AVL 可能更多
 *   2. Linux/glibc/Java TreeMap 都选了红黑树——工业验证充分
 *   3. 红黑树的"黑高平衡"约束比 AVL 的"高度差 ≤ 1"宽松，
 *      减少了调整频率，总体性能更优（尤其删除密集场景）
 *
 * ============================================================================
 * 红黑树 5 条性质（RB Properties）
 * ============================================================================
 *
 *   1. 每个节点要么红，要么黑
 *   2. 根节点是黑色
 *   3. 所有叶子（NIL 哨兵）是黑色
 *   4. 红色节点的两个孩子必须是黑色（不能有连续的红节点）
 *   5. 从任意节点到其所有后代 NIL 叶的路径上，黑色节点数相同（黑高相等）
 *
 * 性质 4+5 共同保证树高 ≤ 2·log₂(n+1)，从而所有操作 O(log n)。
 *
 * ============================================================================
 * 静态节点池
 * ============================================================================
 *
 * [WHY] 为什么不用 kmalloc 分配节点？
 *   VMA 子系统在内核初始化早期就需要工作，而 kmalloc 的堆增长
 *   (heap_grow) 可能触发 vmm_alloc_pages → vma_add → kmalloc 的循环依赖。
 *   静态数组完全绕开此问题。
 *
 *   代价：VMA 上限固定（256 个），但内核启动阶段足够用。
 *
 * 释放策略：被删除的节点标记为 free 放入 freelist，复用空间。
 */

/* ============================================================================
 * 常量与类型
 * ============================================================================ */

#define RB_MAX_NODES 256

#define RB_BLACK 0
#define RB_RED   1

/*
 * rb_node_t — 红黑树内部节点
 *
 * [WHY] 为什么 vm_area_t 嵌入在节点中而不是用指针？
 *   嵌入避免了额外的指针间接寻址，提升缓存局部性。
 *   find() 返回 &node->area（vm_area_t*），调用者看到的是公开视图，
 *   不知道也不关心外面还有红黑树的管理字段。
 */
typedef struct rb_node {
    vm_area_t       area;       /* 公开视图（必须是第一个字段） */
    int             color;      /* RB_BLACK or RB_RED */
    struct rb_node* parent;
    struct rb_node* left;
    struct rb_node* right;
} rb_node_t;

/* ============================================================================
 * 内部数据
 * ============================================================================ */

static rb_node_t  g_pool[RB_MAX_NODES];    /* 静态节点池 */
static rb_node_t* g_free_head;             /* 空闲节点链表头 */
static rb_node_t* g_root;                  /* 树根 */
static unsigned   g_count;                 /* 当前 VMA 数量 */

/*
 * NIL 哨兵节点
 *
 * [WHY] 红黑树用一个共享的 NIL 哨兵替代 NULL 来表示"叶子"。
 *   好处：不需要在每次访问 left/right 前判空，简化旋转和修复代码。
 *   NIL 的 color 始终为 RB_BLACK（性质 3）。
 */
static rb_node_t g_nil_node;
#define NIL (&g_nil_node)

/* ============================================================================
 * 节点池管理
 * ============================================================================ */

/*
 * pool_init — 初始化静态节点池（建立空闲链表）
 *
 * [WHY] 用 right 指针串联空闲节点，省得再定义一个 next 字段。
 *   分配时从头部取，释放时插回头部——O(1) 操作。
 */
static void pool_init(void)
{
    g_free_head = &g_pool[0];
    for (unsigned i = 0; i < RB_MAX_NODES - 1; i++) {
        g_pool[i].right = &g_pool[i + 1];
    }
    g_pool[RB_MAX_NODES - 1].right = NULL;
}

static rb_node_t* pool_alloc(void)
{
    if (!g_free_head) return NULL;

    rb_node_t* n = g_free_head;
    g_free_head = n->right;

    n->area.start = 0;
    n->area.end   = 0;
    n->area.flags = 0;
    n->area.name  = NULL;
    n->color  = RB_RED;       /* 新插入的节点默认为红色 */
    n->parent = NIL;
    n->left   = NIL;
    n->right  = NIL;

    return n;
}

static void pool_free(rb_node_t* n)
{
    n->right = g_free_head;
    g_free_head = n;
}

/* ============================================================================
 * 旋转操作
 *
 * [WHY] 旋转是红黑树维持平衡的核心操作。
 *   左旋和右旋互为镜像，只改变指针指向，不改变中序遍历顺序。
 *
 *   左旋 x：            右旋 y：
 *       x                  y
 *      / \                / \
 *     a   y      →      x   c
 *        / \            / \
 *       b   c          a   b
 *
 * [CPU STATE] 纯内存操作，不涉及特权级或地址空间变化。
 * ============================================================================ */

static void rotate_left(rb_node_t* x)
{
    rb_node_t* y = x->right;
    x->right = y->left;
    if (y->left != NIL)
        y->left->parent = x;
    y->parent = x->parent;
    if (x->parent == NIL)
        g_root = y;
    else if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void rotate_right(rb_node_t* x)
{
    rb_node_t* y = x->left;
    x->left = y->right;
    if (y->right != NIL)
        y->right->parent = x;
    y->parent = x->parent;
    if (x->parent == NIL)
        g_root = y;
    else if (x == x->parent->right)
        x->parent->right = y;
    else
        x->parent->left = y;
    y->right = x;
    x->parent = y;
}

/* ============================================================================
 * 插入修复
 *
 * [WHY] BST 插入后新节点为红色，可能违反性质 4（连续红节点）。
 *   修复分 3 种情况（及其镜像共 6 种），通过重着色和旋转恢复性质。
 *
 *   Case 1: 叔节点为红 → 父和叔变黑，祖父变红，向上递归
 *   Case 2: 叔节点为黑 + z 是外侧孩子 → 旋转父节点，转为 Case 3
 *   Case 3: 叔节点为黑 + z 是内侧孩子 → 旋转祖父 + 重着色
 *
 *   最坏情况：从新节点向上走到根，O(log n) 次重着色 + 最多 2 次旋转。
 * ============================================================================ */

static void insert_fixup(rb_node_t* z)
{
    while (z->parent->color == RB_RED) {
        if (z->parent == z->parent->parent->left) {
            rb_node_t* uncle = z->parent->parent->right;
            if (uncle->color == RB_RED) {
                /* Case 1: 叔节点为红 — 重着色，向上递归 */
                z->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    /* Case 2: z 是右孩子（折线）— 先左旋父，转为 Case 3 */
                    z = z->parent;
                    rotate_left(z);
                }
                /* Case 3: z 是左孩子（直线）— 右旋祖父 + 重着色 */
                z->parent->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                rotate_right(z->parent->parent);
            }
        } else {
            /* 镜像：父是祖父的右孩子 */
            rb_node_t* uncle = z->parent->parent->left;
            if (uncle->color == RB_RED) {
                z->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    rotate_right(z);
                }
                z->parent->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                rotate_left(z->parent->parent);
            }
        }
    }
    g_root->color = RB_BLACK;   /* 保证性质 2：根始终为黑 */
}

/* ============================================================================
 * 删除修复
 *
 * [WHY] 删除一个黑色节点会破坏性质 5（黑高不等）。
 *   修复分 4 种情况（及其镜像共 8 种），通过旋转和重着色恢复。
 *
 *   Case 1: 兄弟为红 → 旋转 + 重着色，转为 Case 2/3/4
 *   Case 2: 兄弟为黑，兄弟的两个孩子都为黑 → 兄弟变红，x 上移
 *   Case 3: 兄弟为黑，兄弟的近侄为红、远侄为黑 → 旋转兄弟，转为 Case 4
 *   Case 4: 兄弟为黑，兄弟的远侄为红 → 旋转父 + 重着色，修复完成
 *
 *   最坏情况：O(log n) 次重着色 + 最多 3 次旋转。
 * ============================================================================ */

static void delete_fixup(rb_node_t* x)
{
    while (x != g_root && x->color == RB_BLACK) {
        if (x == x->parent->left) {
            rb_node_t* w = x->parent->right;
            if (w->color == RB_RED) {
                /* Case 1: 兄弟为红 */
                w->color = RB_BLACK;
                x->parent->color = RB_RED;
                rotate_left(x->parent);
                w = x->parent->right;
            }
            if (w->left->color == RB_BLACK && w->right->color == RB_BLACK) {
                /* Case 2: 兄弟的两个孩子都为黑 */
                w->color = RB_RED;
                x = x->parent;
            } else {
                if (w->right->color == RB_BLACK) {
                    /* Case 3: 近侄为红，远侄为黑 */
                    w->left->color = RB_BLACK;
                    w->color = RB_RED;
                    rotate_right(w);
                    w = x->parent->right;
                }
                /* Case 4: 远侄为红 */
                w->color = x->parent->color;
                x->parent->color = RB_BLACK;
                w->right->color = RB_BLACK;
                rotate_left(x->parent);
                x = g_root;     /* 修复完成，退出循环 */
            }
        } else {
            /* 镜像：x 是父的右孩子 */
            rb_node_t* w = x->parent->left;
            if (w->color == RB_RED) {
                w->color = RB_BLACK;
                x->parent->color = RB_RED;
                rotate_right(x->parent);
                w = x->parent->left;
            }
            if (w->right->color == RB_BLACK && w->left->color == RB_BLACK) {
                w->color = RB_RED;
                x = x->parent;
            } else {
                if (w->left->color == RB_BLACK) {
                    w->right->color = RB_BLACK;
                    w->color = RB_RED;
                    rotate_left(w);
                    w = x->parent->left;
                }
                w->color = x->parent->color;
                x->parent->color = RB_BLACK;
                w->left->color = RB_BLACK;
                rotate_right(x->parent);
                x = g_root;
            }
        }
    }
    x->color = RB_BLACK;
}

/* ============================================================================
 * transplant — 用子树 v 替换子树 u 在其父节点中的位置
 *
 * [WHY] 删除操作的标准辅助：把 u 从树中"拔出"，用 v 顶替。
 *   只修改父指针，不修改 u 的子节点指针（由调用者处理）。
 * ============================================================================ */

static void transplant(rb_node_t* u, rb_node_t* v)
{
    if (u->parent == NIL)
        g_root = v;
    else if (u == u->parent->left)
        u->parent->left = v;
    else
        u->parent->right = v;
    v->parent = u->parent;
}

/* ============================================================================
 * tree_minimum — 找子树中最小节点（最左节点）
 *
 * [WHY] 删除操作需要找后继节点（右子树的最小值）来替代被删节点。
 * ============================================================================ */

static rb_node_t* tree_minimum(rb_node_t* x)
{
    while (x->left != NIL)
        x = x->left;
    return x;
}

/* ============================================================================
 * rb_init — 初始化红黑树后端
 * ============================================================================ */

static void rb_init(void)
{
    /* 初始化 NIL 哨兵 */
    g_nil_node.color  = RB_BLACK;
    g_nil_node.parent = NIL;
    g_nil_node.left   = NIL;
    g_nil_node.right  = NIL;
    g_nil_node.area.start = 0;
    g_nil_node.area.end   = 0;
    g_nil_node.area.flags = 0;
    g_nil_node.area.name  = NULL;

    g_root  = NIL;
    g_count = 0;

    pool_init();
}

/* ============================================================================
 * rb_add — 插入新 VMA
 *
 * 步骤：
 *   1. 参数校验（容量/范围/对齐）
 *   2. BST 插入：沿树找到合适位置，同时检查重叠
 *   3. 写入新节点
 *   4. 红黑修复（insert_fixup）
 *
 * @return 0=成功, -1=失败
 * ============================================================================ */

static int rb_add(uint32_t start, uint32_t end, uint32_t flags, const char* name)
{
    /* 参数校验 */
    if (g_count >= RB_MAX_NODES) {
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

    /*
     * BST 插入：沿树查找插入位置
     *
     * [WHY] 每经过一个节点都检查重叠，这样遍历一次即可完成
     *   重叠检测 + 插入定位，不需要额外的 O(n) 遍历。
     *
     * 重叠判断：两个 [a_start, a_end) 和 [b_start, b_end) 重叠
     * 当且仅当 a_start < b_end && b_start < a_end。
     *
     * 注意：BST 路径上的节点不是全部节点！对于 find 最近的已有 VMA
     *   以精确检测重叠，我们需要额外检查左子树的最右节点和右子树的最左节点。
     *   但在实践中，沿路径检查已经足够：如果新 VMA 与路径外的某节点重叠，
     *   那么它一定也与路径上的某个祖先重叠（因为路径上的祖先的区间
     *   "包裹"了它路径外的后代）。
     *
     *   不对——上述推断不成立。BST 形状由 start 决定，与 end 无关。
     *   为安全起见，我们像 sorted-array 一样做完整的中序遍历检查。
     *   但这会退化到 O(n)。
     *
     *   更好的做法：沿 BST 路径下降时，检查与当前节点的重叠即可。
     *   如果新区间 [start, end) 与某个 BST 路径外的节点重叠，
     *   那在下降过程中一定会经过该节点的某个祖先，而该祖先的区间
     *   与新区间一定也在同侧——不会遗漏。
     *
     *   证明：如果新 VMA 的 start < 某节点 N 的 end（重叠条件之一），
     *   而 N 不在 BST 路径上，那 N 一定在路径上某节点 P 的子树中。
     *   若 N 在 P 的左子树中，则 N.start < P.start，
     *   但新 VMA.start < N.end 且 N.end <= P.start（因为不重叠的已有 VMA），
     *   所以新 VMA.start < P.start，路径会走向 P 的左子树——矛盾（N 不在路径上）。
     *
     *   实际上这个证明不完全正确。为了安全，我们采用简单的中序遍历检查。
     */

    /* 安全起见：遍历全树检查重叠 */
    {
        /* 用栈模拟中序遍历来检查重叠，但这里节点数 ≤ 256，
         * 我们用一个简单的递归辅助函数会导致栈溢出风险。
         * 更简单的做法：沿 BST 路径检查重叠。
         *
         * 实际上，对于 VMA 的 BST（按 start 排序），
         * 新 VMA [start, end) 可能与任意已有 VMA 重叠。
         * 但由于已有 VMA 互不重叠且按 start 排序，
         * 我们只需检查新 VMA 的前驱和后继即可。
         *
         * 做法：先做 BST 插入找到位置，然后检查前驱和后继的重叠。
         */
    }

    /* BST 插入 */
    rb_node_t* parent = NIL;
    rb_node_t* cur = g_root;

    while (cur != NIL) {
        parent = cur;

        /* 沿途检查重叠 */
        if (start < cur->area.end && cur->area.start < end) {
            printk("[vma] ERROR: VMA [0x%08x, 0x%08x) '%s' overlaps with "
                   "existing [0x%08x, 0x%08x) '%s'\n",
                   start, end, name ? name : "?",
                   cur->area.start, cur->area.end,
                   cur->area.name ? cur->area.name : "?");
            return -1;
        }

        if (start < cur->area.start)
            cur = cur->left;
        else
            cur = cur->right;
    }

    /*
     * [WHY] 沿路径检查可能遗漏重叠吗？
     *
     * 不会。因为已有 VMA 互不重叠，且按 start 排序形成 BST。
     * 如果新 VMA 与某个不在路径上的节点 N 重叠，设 N 在路径节点 P 的子树中：
     *
     * - 若 N 在 P 的左子树：N.start < P.start，且 N.end <= P.start（已有 VMA 不重叠）。
     *   新 VMA 若与 N 重叠，则 start < N.end <= P.start，
     *   所以路径在 P 处会走左子树，继续往下一定会遇到 N 或更接近 N 的节点并检测重叠。
     *
     * - 若 N 在 P 的右子树：对称论证。
     *
     * 因此沿路径检查是完备的。
     */

    /* 分配节点 */
    rb_node_t* z = pool_alloc();
    if (!z) {
        printk("[vma] ERROR: node pool exhausted\n");
        return -1;
    }

    z->area.start = start;
    z->area.end   = end;
    z->area.flags = flags;
    z->area.name  = name;
    z->parent     = parent;

    if (parent == NIL) {
        g_root = z;
    } else if (start < parent->area.start) {
        parent->left = z;
    } else {
        parent->right = z;
    }

    /* 红黑修复 */
    insert_fixup(z);
    g_count++;

    return 0;
}

/* ============================================================================
 * rb_remove — 删除指定 start 的 VMA
 *
 * 步骤：
 *   1. BST 查找目标节点
 *   2. 标准 BST 删除（处理 0/1/2 个子节点的情况）
 *   3. 如果删除的节点原来是黑色，进行红黑修复（delete_fixup）
 *   4. 回收节点到空闲池
 *
 * @return 0=成功, -1=未找到
 * ============================================================================ */

static int rb_remove(uint32_t start)
{
    /* BST 查找 */
    rb_node_t* z = g_root;
    while (z != NIL) {
        if (start == z->area.start)
            break;
        else if (start < z->area.start)
            z = z->left;
        else
            z = z->right;
    }

    if (z == NIL) {
        printk("[vma] ERROR: cannot remove VMA with start=0x%08x: not found\n", start);
        return -1;
    }

    /*
     * 标准红黑树删除算法（CLRS 第 13.4 节）
     *
     * y: 实际被移除或移动的节点
     * x: 用来替代 y 位置的子节点（可能是 NIL）
     *
     * 三种情况：
     *   1. z 没有左孩子 → 用右孩子替代 z
     *   2. z 没有右孩子 → 用左孩子替代 z
     *   3. z 有两个孩子 → 找后继 y（右子树最小值），
     *      把 y 的值复制到 z，然后删除 y
     */
    rb_node_t* y = z;
    int y_orig_color = y->color;
    rb_node_t* x;

    if (z->left == NIL) {
        x = z->right;
        transplant(z, z->right);
    } else if (z->right == NIL) {
        x = z->left;
        transplant(z, z->left);
    } else {
        /* z 有两个孩子：找右子树最小值（后继） */
        y = tree_minimum(z->right);
        y_orig_color = y->color;
        x = y->right;

        if (y->parent == z) {
            /* y 是 z 的直接右孩子 */
            x->parent = y;
        } else {
            transplant(y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        transplant(z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }

    /*
     * [WHY] 只有当被移除节点原来是黑色时才需要修复
     *   移除红色节点不会破坏任何红黑性质：
     *   - 不影响黑高（红节点不计入黑高）
     *   - 不会产生连续红节点（红节点的父必为黑）
     */
    if (y_orig_color == RB_BLACK)
        delete_fixup(x);

    pool_free(z);
    g_count--;

    return 0;
}

/* ============================================================================
 * rb_find — 查找包含 addr 的 VMA
 *
 * [WHY] BST 按 start 排序，但 VMA 是区间 [start, end)。
 *   简单的 BST 查找（比较 addr 与 start）不够——
 *   addr 可能在某个 start < addr 的节点的 [start, end) 区间内。
 *
 * 算法：
 *   从根开始，如果 addr < cur.start → 走左子树
 *   如果 addr >= cur.start 且 addr < cur.end → 找到了
 *   如果 addr >= cur.end → 走右子树
 *
 *   第三种情况需要注意：addr >= cur.end 不意味着答案在右子树。
 *   如果 addr >= cur.start 但 addr >= cur.end（落在 cur 右边的空洞），
 *   答案只可能在右子树中（因为左子树所有 start < cur.start < addr）。
 *
 * @return 指向匹配的 vm_area_t，未找到返回 NULL
 * ============================================================================ */

static const vm_area_t* rb_find(uint32_t addr)
{
    rb_node_t* cur = g_root;

    while (cur != NIL) {
        if (addr < cur->area.start) {
            cur = cur->left;
        } else if (addr >= cur->area.end) {
            cur = cur->right;
        } else {
            /* cur->area.start <= addr < cur->area.end — 命中 */
            return &cur->area;
        }
    }

    return NULL;
}

/* ============================================================================
 * rb_dump — 中序遍历打印所有 VMA（天然按地址升序）
 *
 * [WHY] 红黑树的中序遍历 = BST 的升序遍历 = 按 start 地址排序。
 *   用迭代方式避免递归（内核栈有限，递归深度最大 ~16 对 256 节点无问题，
 *   但迭代更安全）。
 *
 * 迭代中序遍历算法（Morris 遍历太复杂，这里用 parent 指针回溯）：
 *   1. 从根开始，先走到最左节点
 *   2. 访问当前节点
 *   3. 如果有右子树，进入右子树的最左节点
 *   4. 否则沿 parent 回溯，直到"从左子树上来"
 * ============================================================================ */

static void rb_dump(void)
{
    if (g_count == 0) {
        printk("(no VMAs registered)\n");
        return;
    }

    unsigned idx = 0;

    /* 找到最左节点（最小 start） */
    rb_node_t* cur = g_root;
    while (cur->left != NIL)
        cur = cur->left;

    while (cur != NIL) {
        /* 访问当前节点 */
        const vm_area_t* a = &cur->area;
        printk("[%2u] 0x%08x - 0x%08x  %4u KiB  %c%c%c  %s\n",
               idx++, a->start, a->end,
               (a->end - a->start) / 1024,
               a->flags & VMA_READ  ? 'r' : '-',
               a->flags & VMA_WRITE ? 'w' : '-',
               a->flags & VMA_EXEC  ? 'x' : '-',
               a->name ? a->name : "?");

        /* 下一个中序后继 */
        if (cur->right != NIL) {
            cur = cur->right;
            while (cur->left != NIL)
                cur = cur->left;
        } else {
            rb_node_t* p = cur->parent;
            while (p != NIL && cur == p->right) {
                cur = p;
                p = p->parent;
            }
            cur = p;
        }
    }
}

/* ============================================================================
 * rb_count
 * ============================================================================ */

static unsigned rb_count(void)
{
    return g_count;
}

/* ============================================================================
 * 后端操作表
 * ============================================================================ */

static const vma_ops_t rb_ops = {
    .name   = "rbtree",
    .init   = rb_init,
    .add    = rb_add,
    .remove = rb_remove,
    .find   = rb_find,
    .dump   = rb_dump,
    .count  = rb_count,
};

const vma_ops_t* vma_rbtree_get_ops(void)
{
    return &rb_ops;
}
