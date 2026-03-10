#include "cmd.h"

#include "pmm.h"
#include "printk.h"

static int cmd_free_main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    unsigned total = pmm_total_pages();
    unsigned free = pmm_free_pages();

    if (total == 0) {
        printk("PMM: not initialized\n");
        return 0;
    }

    unsigned used = total - free;

    /* 1 page = 4KiB; show a human-ish summary without 64-bit printf support. */
    unsigned free_kib = free * 4u;
    unsigned used_kib = used * 4u;

    printk("PMM: free %u / %u pages\n", free, total);
    printk("PMM: free %uKiB, used %uKiB\n", free_kib, used_kib);
    return 0;
}

const cmd_t cmd_free = {
    .name = "free",
    .help = "Show free physical pages (PMM)",
    .fn = cmd_free_main,
};
