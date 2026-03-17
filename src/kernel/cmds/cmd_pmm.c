#include "cmd.h"

#include <stdint.h>
#include <stddef.h>

#include "pmm.h"
#include "printk.h"

#define PMM_PAGE_SIZE 4096u

/*
 * pmm command allocation tracking
 *
 * We only provide `freeall` for pages allocated via `pmm alloc`.
 * This avoids accidentally freeing pages owned by the kernel, drivers,
 * or other subsystems.
 */
#define PMM_CMD_TRACK_MAX 1024u
static uint32_t g_pmm_cmd_allocs[PMM_CMD_TRACK_MAX];
static uint32_t g_pmm_cmd_alloc_count = 0;

static void pmm_cmd_track_add(uint32_t addr) {
    if (g_pmm_cmd_alloc_count < PMM_CMD_TRACK_MAX) {
        g_pmm_cmd_allocs[g_pmm_cmd_alloc_count++] = addr;
    }
}

static int pmm_cmd_track_remove(uint32_t addr) {
    for (uint32_t i = 0; i < g_pmm_cmd_alloc_count; i++) {
        if (g_pmm_cmd_allocs[i] == addr) {
            g_pmm_cmd_alloc_count--;
            g_pmm_cmd_allocs[i] = g_pmm_cmd_allocs[g_pmm_cmd_alloc_count];
            return 1;
        }
    }
    return 0;
}

static int streq(const char* a, const char* b) {
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

/* Parse uint32 from decimal or 0x... hex.
 * Returns 1 on success, 0 on failure.
 */
static int parse_u32(const char* s, uint32_t* out) {
    if (!s || !*s || !out) {
        return 0;
    }

    uint32_t v = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s) {
            return 0;
        }
        while (*s) {
            int hv = hex_val(*s++);
            if (hv < 0) {
                return 0;
            }
            v = (v << 4) | (uint32_t)hv;
        }
        *out = v;
        return 1;
    }

    while (*s) {
        if (!is_digit(*s)) {
            return 0;
        }
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }

    *out = v;
    return 1;
}

static void cmd_pmm_usage(void) {
    printk("Usage:\n");
    printk("  pmm state\n");
    printk("  pmm alloc [count]\n");
    printk("  pmm free <addr>\n");
    printk("  pmm freeall\n");
    printk("  pmm dump [start_index] [count]\n");
    printk("  pmm test              — automated selftest\n");
    printk("Notes:\n");
    printk("  - addr can be decimal or hex (0x...)\n");
    printk("  - dump shows used/free for page indices\n");
}

static int cmd_pmm_stat(void) {
    unsigned total = pmm_total_pages();
    unsigned free = pmm_free_pages();
    unsigned base = pmm_managed_base();

    if (total == 0 || base == 0) {
        printk("PMM: not initialized\n");
        return 0;
    }

    unsigned used = total - free;
    printk("PMM: min_base=0x%08x page_size=%u\n", base, (unsigned)PMM_PAGE_SIZE);
    printk("PMM: free %u / %u pages (used %u)\n", free, total, used);
    return 0;
}

static int cmd_pmm_alloc(int argc, char** argv) {
    uint32_t count = 1;
    if (argc >= 3) {
        if (!parse_u32(argv[2], &count) || count == 0) {
            printk("pmm alloc: invalid count: %s\n", argv[2]);
            return 0;
        }
    }

    /* Keep output reasonable in a tiny serial console. */
    if (count > 256u) {
        printk("pmm alloc: count too large (%u), capped to 256\n", (unsigned)count);
        count = 256u;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (g_pmm_cmd_alloc_count >= PMM_CMD_TRACK_MAX) {
            printk("pmm alloc: tracking full (%u), stop\n", (unsigned)PMM_CMD_TRACK_MAX);
            return 0;
        }
        void* p = pmm_alloc_page();
        if (!p) {
            printk("pmm alloc: OOM after %u pages\n", (unsigned)i);
            return 0;
        }
        pmm_cmd_track_add((uint32_t)(uintptr_t)p);
        printk("pmm alloc: %p\n", p);
    }

    return 0;
}

static int cmd_pmm_free(int argc, char** argv) {
    if (argc < 3) {
        printk("pmm free: missing <addr>\n");
        return 0;
    }

    uint32_t addr = 0;
    if (!parse_u32(argv[2], &addr)) {
        printk("pmm free: invalid addr: %s\n", argv[2]);
        return 0;
    }

    /* Keep tracking consistent: if this address was allocated via `pmm alloc`, untrack it. */
    (void)pmm_cmd_track_remove(addr);
    pmm_free_page((void*)(uintptr_t)addr);
    return 0;
}

static int cmd_pmm_freeall(void) {
    if (g_pmm_cmd_alloc_count == 0) {
        printk("pmm freeall: nothing to free\n");
        return 0;
    }

    uint32_t n = g_pmm_cmd_alloc_count;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr = g_pmm_cmd_allocs[i];
        pmm_free_page((void*)(uintptr_t)addr);
    }
    g_pmm_cmd_alloc_count = 0;
    printk("pmm freeall: freed %u pages\n", (unsigned)n);
    return 0;
}

static int cmd_pmm_dump(int argc, char** argv) {
    unsigned total = pmm_total_pages();
    unsigned base = pmm_managed_base();

    if (total == 0 || base == 0) {
        printk("PMM: not initialized\n");
        return 0;
    }

    uint32_t start = 0;
    uint32_t count = 64;

    if (argc >= 3) {
        if (!parse_u32(argv[2], &start)) {
            printk("pmm dump: invalid start_index: %s\n", argv[2]);
            return 0;
        }
    }
    if (argc >= 4) {
        if (!parse_u32(argv[3], &count) || count == 0) {
            printk("pmm dump: invalid count: %s\n", argv[3]);
            return 0;
        }
    }

    if (count > 256u) {
        printk("pmm dump: count too large (%u), capped to 256\n", (unsigned)count);
        count = 256u;
    }

    if (start >= total) {
        printk("pmm dump: start_index out of range (%u >= %u)\n", (unsigned)start, total);
        return 0;
    }

    unsigned end = (unsigned)start + (unsigned)count;
    if (end > total) {
        end = total;
    }

    printk("PMM dump: index [%u, %u)\n", (unsigned)start, end);

    /* Compact view: one char per page: '#' used, '.' free (32 per line). */
    unsigned line = 0;
    for (unsigned i = (unsigned)start; i < end; i++) {
        int used = pmm_page_is_used(i);
        char c = (used == 1) ? '#' : (used == 0) ? '.' : '?';
        printk("%c", c);
        line++;
        if (line == 32) {
            unsigned idx0 = i - 31u;
            uint32_t addr0 = (uint32_t)pmm_page_addr(idx0);
            if (addr0 == 0) {
                addr0 = (uint32_t)base;
            }
            printk("  idx %u addr 0x%08x\n", idx0, addr0);
            line = 0;
        }
    }

    if (line != 0) {
        unsigned idx0 = end - line;
        uint32_t addr0 = (uint32_t)pmm_page_addr(idx0);
        if (addr0 == 0) {
            addr0 = (uint32_t)base;
        }
        printk("  idx %u addr 0x%08x\n", idx0, addr0);
    }

    return 0;
}

/*
 * pmm test — 自动化 PMM selftest
 *
 * 测试 1: 基本 alloc/free 往返 — 分配后 free 计数减少，释放后恢复
 * 测试 2: 页对齐验证 — 所有返回地址都 4KiB 对齐
 * 测试 3: 唯一性验证 — 连续分配的页地址互不相同
 * 测试 4: 批量压力 — 连续分配 32 页再全部释放，验证计数器一致
 * 测试 5: 写读验证 — 通过 PHYS_TO_VIRT 写入并回读魔数
 */
#include "vmm.h"  /* for PHYS_TO_VIRT */

static int cmd_pmm_test(void) {
    printk("[pmm-test] === PMM selftest ===\n");
    int pass = 1;

    /* --- 测试 1: 基本 alloc/free 往返 --- */
    printk("[pmm-test] test 1: basic alloc/free roundtrip\n");
    unsigned free_before = pmm_free_pages();
    void* p1 = pmm_alloc_page();
    if (!p1) {
        printk("[pmm-test] FAIL: pmm_alloc_page returned NULL\n");
        return 0;
    }
    unsigned free_after_alloc = pmm_free_pages();
    if (free_after_alloc != free_before - 1) {
        printk("[pmm-test] FAIL: free count %u -> %u (expected %u)\n",
               free_before, free_after_alloc, free_before - 1);
        pass = 0;
    }
    pmm_free_page(p1);
    unsigned free_after_free = pmm_free_pages();
    if (free_after_free != free_before) {
        printk("[pmm-test] FAIL: free count not restored %u != %u\n",
               free_after_free, free_before);
        pass = 0;
    }
    if (pass) printk("[pmm-test] PASS\n");

    /* --- 测试 2: 页对齐验证 --- */
    printk("[pmm-test] test 2: page alignment\n");
    int align_ok = 1;
    #define PMM_TEST_ALIGN_N 8
    void* align_pages[PMM_TEST_ALIGN_N];
    for (int i = 0; i < PMM_TEST_ALIGN_N; i++) {
        align_pages[i] = pmm_alloc_page();
        if (!align_pages[i]) {
            printk("[pmm-test] FAIL: alloc returned NULL at i=%d\n", i);
            /* 释放已分配的 */
            for (int j = 0; j < i; j++) pmm_free_page(align_pages[j]);
            return 0;
        }
        uint32_t addr = (uint32_t)(uintptr_t)align_pages[i];
        if (addr & 0xFFFu) {
            printk("[pmm-test] FAIL: page %d not 4KiB aligned: 0x%08x\n", i, addr);
            align_ok = 0;
        }
    }
    for (int i = 0; i < PMM_TEST_ALIGN_N; i++) pmm_free_page(align_pages[i]);
    if (align_ok) {
        printk("[pmm-test] PASS\n");
    } else {
        pass = 0;
    }

    /* --- 测试 3: 唯一性验证 --- */
    printk("[pmm-test] test 3: uniqueness\n");
    int unique_ok = 1;
    #define PMM_TEST_UNIQUE_N 16
    void* unique_pages[PMM_TEST_UNIQUE_N];
    for (int i = 0; i < PMM_TEST_UNIQUE_N; i++) {
        unique_pages[i] = pmm_alloc_page();
        if (!unique_pages[i]) {
            printk("[pmm-test] FAIL: alloc returned NULL at i=%d\n", i);
            for (int j = 0; j < i; j++) pmm_free_page(unique_pages[j]);
            return 0;
        }
    }
    for (int i = 0; i < PMM_TEST_UNIQUE_N; i++) {
        for (int j = i + 1; j < PMM_TEST_UNIQUE_N; j++) {
            if (unique_pages[i] == unique_pages[j]) {
                printk("[pmm-test] FAIL: duplicate page [%d]==[%d]=0x%08x\n",
                       i, j, (unsigned)(uintptr_t)unique_pages[i]);
                unique_ok = 0;
            }
        }
    }
    for (int i = 0; i < PMM_TEST_UNIQUE_N; i++) pmm_free_page(unique_pages[i]);
    if (unique_ok) {
        printk("[pmm-test] PASS\n");
    } else {
        pass = 0;
    }

    /* --- 测试 4: 批量压力 + 计数器一致 --- */
    printk("[pmm-test] test 4: batch stress (32 pages)\n");
    unsigned free_start = pmm_free_pages();
    #define PMM_TEST_BATCH_N 32
    void* batch_pages[PMM_TEST_BATCH_N];
    int alloc_count = 0;
    for (int i = 0; i < PMM_TEST_BATCH_N; i++) {
        batch_pages[i] = pmm_alloc_page();
        if (!batch_pages[i]) {
            printk("[pmm-test] WARN: OOM at page %d (ok if memory is tight)\n", i);
            break;
        }
        alloc_count++;
    }
    unsigned free_mid = pmm_free_pages();
    if (free_mid != free_start - (unsigned)alloc_count) {
        printk("[pmm-test] FAIL: free count %u, expected %u (allocated %d)\n",
               free_mid, free_start - (unsigned)alloc_count, alloc_count);
        pass = 0;
    }
    for (int i = 0; i < alloc_count; i++) {
        pmm_free_page(batch_pages[i]);
    }
    unsigned free_end = pmm_free_pages();
    if (free_end != free_start) {
        printk("[pmm-test] FAIL: free count not restored %u != %u\n",
               free_end, free_start);
        pass = 0;
    }
    if (pass) printk("[pmm-test] PASS\n");

    /* --- 测试 5: 写读验证（通过内核虚拟地址） --- */
    printk("[pmm-test] test 5: write/read via PHYS_TO_VIRT\n");
    if (vmm_is_ready()) {
        void* pg = pmm_alloc_page();
        if (pg) {
            uint32_t* virt = (uint32_t*)PHYS_TO_VIRT(pg);
            /* 写入魔数模式 */
            for (unsigned i = 0; i < PMM_PAGE_SIZE / sizeof(uint32_t); i++) {
                virt[i] = 0xC0FFEE00u + i;
            }
            /* 回读验证 */
            int rw_ok = 1;
            for (unsigned i = 0; i < PMM_PAGE_SIZE / sizeof(uint32_t); i++) {
                if (virt[i] != 0xC0FFEE00u + i) {
                    printk("[pmm-test] FAIL: mismatch at word %u: 0x%08x != 0x%08x\n",
                           i, virt[i], 0xC0FFEE00u + i);
                    rw_ok = 0;
                    break;
                }
            }
            pmm_free_page(pg);
            if (rw_ok) {
                printk("[pmm-test] PASS\n");
            } else {
                pass = 0;
            }
        } else {
            printk("[pmm-test] SKIP: alloc returned NULL\n");
        }
    } else {
        printk("[pmm-test] SKIP: VMM not ready\n");
    }

    printk("[pmm-test] === %s ===\n", pass ? "ALL PASS" : "SOME FAILED");
    return 0;
}

static int cmd_pmm_main(int argc, char** argv) {
    if (argc < 2) {
        cmd_pmm_usage();
        return 0;
    }

    if (streq(argv[1], "state") || streq(argv[1], "stat")) {
        return cmd_pmm_stat();
    }
    if (streq(argv[1], "alloc")) {
        return cmd_pmm_alloc(argc, argv);
    }
    if (streq(argv[1], "free")) {
        return cmd_pmm_free(argc, argv);
    }
    if (streq(argv[1], "freeall")) {
        return cmd_pmm_freeall();
    }
    if (streq(argv[1], "dump")) {
        return cmd_pmm_dump(argc, argv);
    }
    if (streq(argv[1], "test")) {
        return cmd_pmm_test();
    }
    if (streq(argv[1], "help") || streq(argv[1], "-h") || streq(argv[1], "--help")) {
        cmd_pmm_usage();
        return 0;
    }

    printk("pmm: unknown subcommand: %s\n", argv[1]);
    cmd_pmm_usage();
    return 0;
}

const cmd_t cmd_pmm = {
    .name = "pmm",
    .help = "PMM tools: state/alloc/free/freeall/dump/test",
    .fn = cmd_pmm_main,
};
