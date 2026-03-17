#include "cmd.h"

#include <stdint.h>
#include <stddef.h>

#include "kmalloc.h"
#include "printk.h"

/*
 * cmd_heap — 内核堆 (kmalloc/kfree) 测试命令
 *
 * 用法：
 *   heap status              — 显示堆统计信息
 *   heap alloc <bytes>       — kmalloc(bytes)，显示返回地址
 *   heap free                — kfree 上次分配的指针
 *   heap test                — 自动化测试（alloc → write → read → free → stats 验证）
 */

/* 最多记录 8 个分配，方便连续测试 */
#define MAX_SLOTS 8
static void*    s_slots[MAX_SLOTS];
static unsigned s_sizes[MAX_SLOTS];
static unsigned s_slot_count = 0;

static int streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static unsigned parse_dec(const char* s) {
    if (!s || !*s) return 0;
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned)(*s - '0');
        s++;
    }
    return v;
}

/* ---- status ---- */
static void show_status(void) {
    const char* name = kmalloc_backend_name();
    printk("Heap backend: %s\n", name ? name : "(none)");

    kheap_stats_t st;
    kheap_get_stats(&st);
    printk("Heap size:    %u bytes (%u KiB)\n",
           (unsigned)st.heap_size, (unsigned)(st.heap_size / 1024u));
    printk("Used:         %u bytes (%u allocs)\n",
           (unsigned)st.used_bytes, st.alloc_count);
    printk("Free:         %u bytes (%u blocks)\n",
           (unsigned)st.free_bytes, st.free_count);

    printk("Tracked slots: %u / %u\n", s_slot_count, MAX_SLOTS);
    for (unsigned i = 0; i < s_slot_count; i++) {
        printk("  [%u] 0x%08x  %u bytes\n",
               i, (unsigned)(uintptr_t)s_slots[i], s_sizes[i]);
    }
}

/* ---- alloc ---- */
static void do_alloc(unsigned bytes) {
    if (bytes == 0) {
        printk("Usage: heap alloc <bytes>\n");
        return;
    }
    if (s_slot_count >= MAX_SLOTS) {
        printk("All %u slots used — run 'heap free' first\n", MAX_SLOTS);
        return;
    }

    void* ptr = kmalloc(bytes);
    if (!ptr) {
        printk("[heap] FAIL: kmalloc(%u) returned NULL\n", bytes);
        return;
    }

    printk("[heap] OK: kmalloc(%u) = 0x%08x\n", bytes, (unsigned)(uintptr_t)ptr);

    /* 写入魔数验证可写可读 */
    uint8_t* p = (uint8_t*)ptr;
    for (unsigned i = 0; i < bytes; i++) {
        p[i] = (uint8_t)(i & 0xFF);
    }
    for (unsigned i = 0; i < bytes; i++) {
        if (p[i] != (uint8_t)(i & 0xFF)) {
            printk("[heap] FAIL: readback mismatch at offset %u\n", i);
            return;
        }
    }
    printk("[heap] write/read %u bytes: PASS\n", bytes);

    s_slots[s_slot_count] = ptr;
    s_sizes[s_slot_count] = bytes;
    s_slot_count++;
}

/* ---- free ---- */
static void do_free(void) {
    if (s_slot_count == 0) {
        printk("Nothing to free — run 'heap alloc <bytes>' first\n");
        return;
    }

    /* 释放最后一个 slot (LIFO) */
    s_slot_count--;
    void* ptr = s_slots[s_slot_count];
    unsigned sz = s_sizes[s_slot_count];

    printk("[heap] kfree(0x%08x) (%u bytes)\n", (unsigned)(uintptr_t)ptr, sz);
    kfree(ptr);
    printk("[heap] OK\n");
}

/* ---- test ---- */
static void do_test(void) {
    /*
     * 自动化测试 — 验证 kmalloc/kfree 基本功能：
     *
     * 测试 1: 单次 alloc + write + read + free
     * 测试 2: 多次 alloc 不同大小，验证地址不重叠
     * 测试 3: 全部 free 后 stats 验证 alloc_count == 0
     * 测试 4: free 后重新 alloc，验证空间可复用
     * 测试 5: 对齐验证 — 所有返回指针至少 8 字节对齐
     * 测试 6: 碎片化压力 — 交替 alloc/free，验证碎片回收
     * 测试 7: 递增大小 — 从 1 到 2048 字节逐级分配
     * 测试 8: 大块分配 — 4096 字节整页级分配
     */
    printk("[heap-test] === kmalloc/kfree selftest ===\n");

    /* 先清理之前的 slots */
    while (s_slot_count > 0) {
        s_slot_count--;
        kfree(s_slots[s_slot_count]);
    }

    kheap_stats_t st;
    int pass = 1;

    /* --- 测试 1: 基本 alloc/free --- */
    printk("[heap-test] test 1: basic alloc/free\n");
    void* p1 = kmalloc(64);
    if (!p1) { printk("[heap-test] FAIL: kmalloc(64) returned NULL\n"); return; }
    printk("[heap-test] kmalloc(64)  = 0x%08x\n", (unsigned)(uintptr_t)p1);

    /* 写满 64 字节 */
    uint8_t* b = (uint8_t*)p1;
    for (int i = 0; i < 64; i++) b[i] = (uint8_t)i;
    for (int i = 0; i < 64; i++) {
        if (b[i] != (uint8_t)i) {
            printk("[heap-test] FAIL: readback mismatch at byte %d\n", i);
            return;
        }
    }
    printk("[heap-test] write/read 64 bytes: PASS\n");

    kfree(p1);
    printk("[heap-test] kfree: OK\n");

    /* --- 测试 2: 多次分配不同大小 --- */
    printk("[heap-test] test 2: multiple allocs (32, 128, 256, 1024)\n");
    unsigned sizes[] = { 32, 128, 256, 1024 };
    void*    ptrs[4];

    for (int i = 0; i < 4; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i]) {
            printk("[heap-test] FAIL: kmalloc(%u) returned NULL\n", sizes[i]);
            return;
        }
        printk("[heap-test] kmalloc(%4u) = 0x%08x\n", sizes[i],
               (unsigned)(uintptr_t)ptrs[i]);

        /* 写入魔数 */
        uint32_t* w = (uint32_t*)ptrs[i];
        w[0] = 0xAA550000u + i;
    }

    /* 验证地址不重叠：每个分配的首字魔数还在 */
    for (int i = 0; i < 4; i++) {
        uint32_t* w = (uint32_t*)ptrs[i];
        if (w[0] != 0xAA550000u + (unsigned)i) {
            printk("[heap-test] FAIL: ptr[%d] overwritten (0x%08x != 0x%08x)\n",
                   i, w[0], 0xAA550000u + (unsigned)i);
            return;
        }
    }
    printk("[heap-test] no overlap: PASS\n");

    /* --- 测试 3: 全部 free，检查 stats --- */
    printk("[heap-test] test 3: free all, check stats\n");
    for (int i = 0; i < 4; i++) {
        kfree(ptrs[i]);
    }

    kheap_get_stats(&st);
    printk("[heap-test] after free: alloc_count=%u, free_bytes=%u\n",
           st.alloc_count, (unsigned)st.free_bytes);
    if (st.alloc_count != 0) {
        printk("[heap-test] FAIL: alloc_count should be 0, got %u\n", st.alloc_count);
        pass = 0;
    } else {
        printk("[heap-test] alloc_count == 0: PASS\n");
    }

    /* --- 测试 4: 复用验证 --- */
    printk("[heap-test] test 4: reuse after free\n");
    void* p2 = kmalloc(64);
    if (!p2) { printk("[heap-test] FAIL: re-alloc returned NULL\n"); return; }
    printk("[heap-test] re-alloc kmalloc(64) = 0x%08x\n", (unsigned)(uintptr_t)p2);

    uint32_t* w2 = (uint32_t*)p2;
    w2[0] = 0xDEADBEEFu;
    if (w2[0] != 0xDEADBEEFu) {
        printk("[heap-test] FAIL: write/read after re-alloc\n");
        pass = 0;
    } else {
        printk("[heap-test] reuse: PASS\n");
    }
    kfree(p2);

    /* --- 测试 5: 对齐验证 --- */
    printk("[heap-test] test 5: alignment (8-byte minimum)\n");
    {
        unsigned align_sizes[] = { 1, 3, 7, 13, 33, 100, 255, 512 };
        int align_n = (int)(sizeof(align_sizes) / sizeof(align_sizes[0]));
        void* align_ptrs[8];
        int align_ok = 1;

        for (int i = 0; i < align_n; i++) {
            align_ptrs[i] = kmalloc(align_sizes[i]);
            if (!align_ptrs[i]) {
                printk("[heap-test] FAIL: kmalloc(%u) returned NULL\n", align_sizes[i]);
                for (int j = 0; j < i; j++) kfree(align_ptrs[j]);
                return;
            }
            uintptr_t addr = (uintptr_t)align_ptrs[i];
            if (addr & 0x7u) {
                printk("[heap-test] FAIL: kmalloc(%u) = 0x%08x not 8-byte aligned\n",
                       align_sizes[i], (unsigned)addr);
                align_ok = 0;
            }
        }
        for (int i = 0; i < align_n; i++) kfree(align_ptrs[i]);
        if (align_ok) {
            printk("[heap-test] PASS\n");
        } else {
            pass = 0;
        }
    }

    /* --- 测试 6: 碎片化压力 — alloc 8 块, free 偶数, alloc 填回 --- */
    printk("[heap-test] test 6: fragmentation stress\n");
    {
        #define FRAG_N 8
        void* frag[FRAG_N];
        for (int i = 0; i < FRAG_N; i++) {
            frag[i] = kmalloc(64);
            if (!frag[i]) {
                printk("[heap-test] FAIL: frag alloc %d returned NULL\n", i);
                for (int j = 0; j < i; j++) kfree(frag[j]);
                return;
            }
            /* 写入标记 */
            ((uint32_t*)frag[i])[0] = 0xF0F00000u + (unsigned)i;
        }

        /* 释放偶数位置（制造碎片空洞） */
        for (int i = 0; i < FRAG_N; i += 2) {
            kfree(frag[i]);
            frag[i] = NULL;
        }

        /* 验证奇数位置数据完好 */
        int frag_ok = 1;
        for (int i = 1; i < FRAG_N; i += 2) {
            uint32_t val = ((uint32_t*)frag[i])[0];
            if (val != 0xF0F00000u + (unsigned)i) {
                printk("[heap-test] FAIL: frag[%d] corrupted: 0x%08x\n", i, val);
                frag_ok = 0;
            }
        }

        /* 重新填充偶数位置（碎片回收） */
        for (int i = 0; i < FRAG_N; i += 2) {
            frag[i] = kmalloc(64);
            if (!frag[i]) {
                printk("[heap-test] FAIL: frag re-alloc %d returned NULL\n", i);
                frag_ok = 0;
                break;
            }
            ((uint32_t*)frag[i])[0] = 0xBEBE0000u + (unsigned)i;
        }

        /* 再次验证所有数据 */
        for (int i = 0; i < FRAG_N; i++) {
            if (!frag[i]) continue;
            uint32_t expected;
            if (i % 2 == 0)
                expected = 0xBEBE0000u + (unsigned)i;
            else
                expected = 0xF0F00000u + (unsigned)i;
            uint32_t actual = ((uint32_t*)frag[i])[0];
            if (actual != expected) {
                printk("[heap-test] FAIL: frag[%d] = 0x%08x, expected 0x%08x\n",
                       i, actual, expected);
                frag_ok = 0;
            }
        }

        for (int i = 0; i < FRAG_N; i++) {
            if (frag[i]) kfree(frag[i]);
        }

        if (frag_ok) {
            printk("[heap-test] PASS\n");
        } else {
            pass = 0;
        }
        #undef FRAG_N
    }

    /* --- 测试 7: 递增大小分配 --- */
    printk("[heap-test] test 7: increasing sizes (1..2048)\n");
    {
        unsigned inc_sizes[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048 };
        int inc_n = (int)(sizeof(inc_sizes) / sizeof(inc_sizes[0]));
        int inc_ok = 1;

        for (int i = 0; i < inc_n; i++) {
            void* p = kmalloc(inc_sizes[i]);
            if (!p) {
                printk("[heap-test] FAIL: kmalloc(%u) returned NULL\n", inc_sizes[i]);
                inc_ok = 0;
                break;
            }
            /* 写满并回读 */
            uint8_t* bp = (uint8_t*)p;
            uint8_t pattern = (uint8_t)(inc_sizes[i] & 0xFF);
            for (unsigned j = 0; j < inc_sizes[i]; j++) bp[j] = pattern;
            for (unsigned j = 0; j < inc_sizes[i]; j++) {
                if (bp[j] != pattern) {
                    printk("[heap-test] FAIL: kmalloc(%u) readback error at byte %u\n",
                           inc_sizes[i], j);
                    inc_ok = 0;
                    break;
                }
            }
            kfree(p);
            if (!inc_ok) break;
        }
        if (inc_ok) {
            printk("[heap-test] PASS\n");
        } else {
            pass = 0;
        }
    }

    /* --- 测试 8: 大块分配（4096 字节） --- */
    printk("[heap-test] test 8: large allocation (4096 bytes)\n");
    {
        void* big = kmalloc(4096);
        if (!big) {
            printk("[heap-test] FAIL: kmalloc(4096) returned NULL\n");
            pass = 0;
        } else {
            /* 写满整页 */
            uint32_t* wp = (uint32_t*)big;
            int big_ok = 1;
            for (unsigned i = 0; i < 4096 / sizeof(uint32_t); i++) {
                wp[i] = 0xCAFE0000u + i;
            }
            for (unsigned i = 0; i < 4096 / sizeof(uint32_t); i++) {
                if (wp[i] != 0xCAFE0000u + i) {
                    printk("[heap-test] FAIL: 4096-byte readback mismatch at word %u\n", i);
                    big_ok = 0;
                    break;
                }
            }

            kheap_get_stats(&st);
            if (st.alloc_count < 1) {
                printk("[heap-test] FAIL: alloc_count should be >= 1 after 4096 alloc\n");
                big_ok = 0;
            }

            kfree(big);
            if (big_ok) {
                printk("[heap-test] PASS\n");
            } else {
                pass = 0;
            }
        }
    }

    printk("[heap-test] === %s ===\n", pass ? "ALL PASS" : "SOME FAILED");
}

static int cmd_heap_main(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: heap <status|alloc|free|test>\n");
        printk("  heap status        — show heap stats\n");
        printk("  heap alloc <bytes> — kmalloc N bytes\n");
        printk("  heap free          — kfree last alloc\n");
        printk("  heap test          — automated selftest\n");
        return 0;
    }

    if (streq(argv[1], "status")) {
        show_status();
    } else if (streq(argv[1], "alloc")) {
        unsigned bytes = (argc >= 3) ? parse_dec(argv[2]) : 0;
        do_alloc(bytes);
    } else if (streq(argv[1], "free")) {
        do_free();
    } else if (streq(argv[1], "test")) {
        do_test();
    } else {
        printk("Unknown subcommand: %s\n", argv[1]);
    }
    return 0;
}

const cmd_t cmd_heap = {
    .name = "heap",
    .help = "VMM page alloc/free test (heap status|alloc|free|test)",
    .fn = cmd_heap_main,
};
