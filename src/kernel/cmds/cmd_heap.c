#include "cmd.h"

#include <stdint.h>
#include <stddef.h>

#include "vmm.h"
#include "pmm.h"
#include "printk.h"

/*
 * cmd_heap — 内核堆/VMM alloc 测试命令
 *
 * 用法：
 *   heap status          — 显示 VMM alloc 区域信息和 PMM 空闲页
 *   heap alloc <pages>   — 调 vmm_alloc_pages 分配 N 页并验证可写
 *   heap free            — 释放上次 alloc 分配的页
 *   heap test            — 自动化测试：alloc → 写入 → 读回验证 → free → 检查 PMM
 */

/* 保存上次分配的地址和页数，供 free 使用 */
static void* s_last_alloc = NULL;
static unsigned s_last_count = 0;

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

static void show_status(void) {
    unsigned free_pages = pmm_free_pages();
    unsigned total_pages = pmm_total_pages();
    printk("PMM: %u / %u pages free (%u KiB free)\n",
           free_pages, total_pages, free_pages * 4u);
    printk("VMM: alloc area ready = %s\n", vmm_is_ready() ? "yes" : "no");
    if (s_last_alloc) {
        printk("Last alloc: 0x%08x (%u pages)\n",
               (unsigned)(uintptr_t)s_last_alloc, s_last_count);
    } else {
        printk("Last alloc: (none)\n");
    }
}

static void do_alloc(unsigned pages) {
    if (pages == 0) {
        printk("Usage: heap alloc <pages>\n");
        return;
    }
    if (s_last_alloc) {
        printk("Already allocated at 0x%08x — run 'heap free' first\n",
               (unsigned)(uintptr_t)s_last_alloc);
        return;
    }

    unsigned free_before = pmm_free_pages();
    printk("[heap] allocating %u pages...\n", pages);

    void* ptr = vmm_alloc_pages(pages);
    if (!ptr) {
        printk("[heap] FAIL: vmm_alloc_pages returned NULL\n");
        return;
    }

    unsigned free_after = pmm_free_pages();
    printk("[heap] OK: vaddr=0x%08x, PMM %u -> %u (-%u pages)\n",
           (unsigned)(uintptr_t)ptr, free_before, free_after,
           free_before - free_after);

    /* 验证每页可写可读 */
    uint32_t* base = (uint32_t*)ptr;
    for (unsigned i = 0; i < pages; i++) {
        uint32_t* page_start = (uint32_t*)((uint8_t*)base + i * 4096u);
        page_start[0] = 0xDEAD0000u + i;   /* 写入魔数 */
    }
    for (unsigned i = 0; i < pages; i++) {
        uint32_t* page_start = (uint32_t*)((uint8_t*)base + i * 4096u);
        uint32_t expected = 0xDEAD0000u + i;
        if (page_start[0] != expected) {
            printk("[heap] FAIL: page %u readback 0x%08x != expected 0x%08x\n",
                   i, page_start[0], expected);
            return;
        }
    }
    printk("[heap] write/read verification: PASS (%u pages)\n", pages);

    s_last_alloc = ptr;
    s_last_count = pages;
}

static void do_free(void) {
    if (!s_last_alloc) {
        printk("Nothing to free — run 'heap alloc <pages>' first\n");
        return;
    }

    unsigned free_before = pmm_free_pages();
    printk("[heap] freeing %u pages at 0x%08x...\n",
           s_last_count, (unsigned)(uintptr_t)s_last_alloc);

    vmm_free_pages(s_last_alloc, s_last_count);

    unsigned free_after = pmm_free_pages();
    printk("[heap] OK: PMM %u -> %u (+%u pages)\n",
           free_before, free_after, free_after - free_before);

    /* 验证释放的页数量正确 */
    if (free_after - free_before != s_last_count) {
        printk("[heap] WARN: expected +%u pages but got +%u\n",
               s_last_count, free_after - free_before);
    }

    s_last_alloc = NULL;
    s_last_count = 0;
}

static void do_test(void) {
    /*
     * 自动化测试流程：
     *   1. 记录 PMM free pages
     *   2. alloc 4 页
     *   3. 每页写入魔数，读回验证
     *   4. free
     *   5. 检查 PMM free pages 恢复
     */
    unsigned test_pages = 4;

    printk("[heap-test] === vmm_alloc_pages selftest ===\n");

    if (s_last_alloc) {
        printk("[heap-test] cleaning up previous alloc first\n");
        do_free();
    }

    unsigned free_before = pmm_free_pages();
    printk("[heap-test] PMM free before: %u pages\n", free_before);

    /* alloc */
    void* ptr = vmm_alloc_pages(test_pages);
    if (!ptr) {
        printk("[heap-test] FAIL: vmm_alloc_pages(%u) returned NULL\n", test_pages);
        return;
    }
    unsigned free_mid = pmm_free_pages();
    printk("[heap-test] alloc %u pages at 0x%08x, PMM free: %u (-%u)\n",
           test_pages, (unsigned)(uintptr_t)ptr,
           free_mid, free_before - free_mid);

    /*
     * [WHY] 消耗的页数 >= test_pages，因为 vmm_map_page 内部
     * 可能额外分配物理页来创建新的页表（vmm_ensure_page_table）。
     * 每个页表覆盖 4MiB（1024 个 PTE），所以开销很小。
     */
    if (free_before - free_mid < test_pages) {
        printk("[heap-test] FAIL: expected >=-%u pages, got -%u\n",
               test_pages, free_before - free_mid);
        return;
    }
    unsigned pt_overhead = (free_before - free_mid) - test_pages;
    if (pt_overhead > 0) {
        printk("[heap-test] (page table overhead: %u extra pages)\n", pt_overhead);
    }

    /* write + readback */
    for (unsigned i = 0; i < test_pages; i++) {
        uint32_t* p = (uint32_t*)((uint8_t*)ptr + i * 4096u);
        p[0] = 0xCAFE0000u + i;
        p[1023] = 0xBEEF0000u + i;  /* 页末尾也写一下 */
    }
    for (unsigned i = 0; i < test_pages; i++) {
        uint32_t* p = (uint32_t*)((uint8_t*)ptr + i * 4096u);
        if (p[0] != 0xCAFE0000u + i || p[1023] != 0xBEEF0000u + i) {
            printk("[heap-test] FAIL: page %u readback mismatch\n", i);
            return;
        }
    }
    printk("[heap-test] write/read: PASS\n");

    /* free */
    vmm_free_pages(ptr, test_pages);
    unsigned free_after = pmm_free_pages();
    printk("[heap-test] free done, PMM free: %u (+%u)\n",
           free_after, free_after - free_mid);

    /*
     * [WHY] free 后 PMM 应恢复 test_pages 页，但页表本身不会被释放
     * （vmm_free_pages 只释放数据页，不回收页表）。
     * 所以 free_after = free_before - pt_overhead。
     */
    unsigned leaked = free_before - free_after;
    if (free_after + pt_overhead != free_before) {
        printk("[heap-test] FAIL: PMM free %u, expected %u (leak %u pages)\n",
               free_after, free_before - pt_overhead, leaked - pt_overhead);
        return;
    }

    printk("[heap-test] === ALL PASS ===\n");
}

static int cmd_heap_main(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: heap <status|alloc|free|test>\n");
        printk("  heap status         — show VMM alloc area & PMM state\n");
        printk("  heap alloc <pages>  — allocate N virtual pages\n");
        printk("  heap free           — free last allocation\n");
        printk("  heap test           — automated alloc/write/free selftest\n");
        return 0;
    }

    if (streq(argv[1], "status")) {
        show_status();
    } else if (streq(argv[1], "alloc")) {
        unsigned pages = (argc >= 3) ? parse_dec(argv[2]) : 0;
        do_alloc(pages);
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
