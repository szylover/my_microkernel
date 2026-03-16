#include "cmd.h"

#include <stdint.h>
#include <stddef.h>

#include "vma.h"
#include "printk.h"

/* ============================================================================
 * 小工具（和 cmd_pmm.c / cmd_vmm.c 统一风格）
 * ============================================================================ */

static int streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int parse_u32(const char* s, uint32_t* out) {
    if (!s || !*s || !out) return 0;
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s) return 0;
        while (*s) {
            int hv = hex_val(*s++);
            if (hv < 0) return 0;
            v = (v << 4) | (uint32_t)hv;
        }
        *out = v;
        return 1;
    }
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    *out = v;
    return 1;
}

/* ============================================================================
 * 子命令
 * ============================================================================ */

static void cmd_vma_usage(void) {
    printk("Usage:\n");
    printk("  vma list              List all VMAs (sorted by address)\n");
    printk("  vma find <addr>       Find VMA containing address\n");
    printk("  vma count             Show VMA count and backend info\n");
    printk("  vma test              Run VMA selftest\n");
    printk("Notes:\n");
    printk("  Addresses can be decimal or hex (0x...)\n");
}

static int cmd_vma_list(void) {
    if (!vma_is_ready()) {
        printk("VMA subsystem not initialized\n");
        return 0;
    }
    vma_dump();
    return 0;
}

static int cmd_vma_find_addr(int argc, char** argv) {
    if (argc < 3) {
        printk("vma find: missing <addr>\n");
        return 0;
    }

    uint32_t addr = 0;
    if (!parse_u32(argv[2], &addr)) {
        printk("vma find: invalid addr: %s\n", argv[2]);
        return 0;
    }

    const vm_area_t* vma = vma_find(addr);
    if (vma) {
        uint32_t size = vma->end - vma->start;
        uint32_t offset = addr - vma->start;
        printk("0x%08x -> VMA [0x%08x, 0x%08x) '%s'\n",
               addr, vma->start, vma->end,
               vma->name ? vma->name : "?");
        printk("  size: %u KiB, offset: 0x%x, flags: %c%c%c\n",
               size / 1024u, offset,
               (vma->flags & VMA_READ)  ? 'r' : '-',
               (vma->flags & VMA_WRITE) ? 'w' : '-',
               (vma->flags & VMA_EXEC)  ? 'x' : '-');
    } else {
        printk("0x%08x -> no VMA (unmapped / untracked)\n", addr);
    }
    return 0;
}

static int cmd_vma_count_fn(void) {
    if (!vma_is_ready()) {
        printk("VMA subsystem not initialized\n");
        return 0;
    }
    const char* backend = vma_backend_name();
    printk("VMA backend: %s\n", backend ? backend : "(none)");
    printk("VMA count:   %u\n", vma_count());
    return 0;
}

/*
 * vma test — selftest
 *
 * [WHY] 验证 VMA 后端的基本功能：
 *   1. add + find: 添加一个临时 VMA，然后查找确认存在
 *   2. find outside: 查找不属于该 VMA 的地址，确认返回 NULL
 *   3. remove + find: 移除后再查找，确认不存在
 *   4. overlap: 尝试添加重叠的 VMA，确认被拒绝
 *
 * 测试用地址 0xF1000000 - 0xF1010000 (64KiB)，位于内核虚拟地址空间的
 * 高端区域，不会和已有映射冲突。
 */
static int cmd_vma_test(void) {
    if (!vma_is_ready()) {
        printk("VMA subsystem not initialized\n");
        return 0;
    }

    printk("[vma-test] === VMA selftest ===\n");

    /* 测试区域: [0xF1000000, 0xF1010000)  64KiB */
    const uint32_t test_start = 0xF1000000u;
    const uint32_t test_end   = 0xF1010000u;
    int pass = 1;

    /* test 1: add + find */
    printk("[vma-test] test 1: add + find\n");
    if (vma_add(test_start, test_end, VMA_READ | VMA_WRITE, "_test") != 0) {
        printk("[vma-test] FAIL: vma_add returned error\n");
        return 0;
    }
    const vm_area_t* v = vma_find(test_start + 0x100);
    if (!v || v->start != test_start || v->end != test_end) {
        printk("[vma-test] FAIL: vma_find inside range failed\n");
        pass = 0;
    } else {
        printk("[vma-test] PASS\n");
    }

    /* test 2: find outside */
    printk("[vma-test] test 2: find outside\n");
    v = vma_find(test_start - 0x1000);
    if (v && v->start == test_start) {
        printk("[vma-test] FAIL: found VMA for address below range\n");
        pass = 0;
    } else {
        printk("[vma-test] PASS\n");
    }

    /* test 3: overlap detection */
    printk("[vma-test] test 3: overlap detection\n");
    int ret = vma_add(test_start + 0x1000, test_end + 0x1000,
                      VMA_READ, "_test_overlap");
    if (ret == 0) {
        printk("[vma-test] FAIL: overlapping add should have failed\n");
        /* clean up the accidentally added VMA */
        vma_remove(test_start + 0x1000);
        pass = 0;
    } else {
        printk("[vma-test] PASS\n");
    }

    /* test 4: remove + find */
    printk("[vma-test] test 4: remove + find\n");
    if (vma_remove(test_start) != 0) {
        printk("[vma-test] FAIL: vma_remove returned error\n");
        pass = 0;
    } else {
        v = vma_find(test_start + 0x100);
        if (v && v->start == test_start) {
            printk("[vma-test] FAIL: VMA still found after remove\n");
            pass = 0;
        } else {
            printk("[vma-test] PASS\n");
        }
    }

    printk("[vma-test] === %s ===\n", pass ? "ALL PASS" : "SOME FAILED");
    return 0;
}

/* ============================================================================
 * 命令入口
 * ============================================================================ */

static int cmd_vma_fn(int argc, char** argv) {
    if (argc < 2) {
        cmd_vma_usage();
        return 0;
    }

    if (streq(argv[1], "list"))  return cmd_vma_list();
    if (streq(argv[1], "find"))  return cmd_vma_find_addr(argc, argv);
    if (streq(argv[1], "count")) return cmd_vma_count_fn();
    if (streq(argv[1], "test"))  return cmd_vma_test();

    cmd_vma_usage();
    return 0;
}

const cmd_t cmd_vma = {
    .name = "vma",
    .help = "Virtual memory area management (list/find/count/test)",
    .fn   = cmd_vma_fn,
};
