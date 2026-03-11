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
    .help = "PMM tools: state/alloc/free/freeall/dump",
    .fn = cmd_pmm_main,
};
