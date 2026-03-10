#include <stdint.h>
#include <stddef.h>

#include "printk.h"

/*
 * We store the Multiboot2 information pointer in kmain.c.
 * This keeps the boot entry signature unchanged and makes the data accessible
 * from shell commands.
 */
extern const void* g_mb2_info;

/* Multiboot2 tag header (common prefix for all tags). */
struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

/* Tag type numbers (Multiboot2). */
enum {
    MB2_TAG_TYPE_END  = 0,
    MB2_TAG_TYPE_MMAP = 6,
};

/* Multiboot2 Memory Map tag (type = 6). */
struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* Followed by entries[] */
} __attribute__((packed));

/* Multiboot2 memory map entry (standard part). */
struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

static uint32_t align8_u32(uint32_t n) { return (n + 7u) & ~7u; }

/* printk() supports only 32-bit %x; print 64-bit as 0xHHHHHHHHLLLLLLLL. */
static void printk_hex_u64(uint64_t v) {
    uint32_t hi = (uint32_t)(v >> 32);
    uint32_t lo = (uint32_t)(v & 0xffffffffu);

    printk("0x%x%08x", hi, lo);
}

static uint64_t u64_add_sat(uint64_t a, uint64_t b) {
    uint64_t r = a + b;
    if (r < a) {
        return UINT64_MAX;
    }
    return r;
}

static void printk_hex_range(uint64_t start, uint64_t end) {
    /* Keep the summary format readable in 32-bit setups. */
    if ((start >> 32) == 0 && (end >> 32) == 0) {
        printk("0x%x - 0x%x", (uint32_t)start, (uint32_t)end);
        return;
    }

    printk_hex_u64(start);
    printk(" - ");
    printk_hex_u64(end);
}

enum mb2_find_result {
    MB2_FIND_OK = 0,
    MB2_FIND_NOT_FOUND,
    MB2_FIND_INVALID,
};

static enum mb2_find_result mb2_find_tag(const uint8_t* base, uint32_t total_size, uint32_t want_type, const struct mb2_tag** out) {
    if (!base || total_size < 16 || !out) {
        return MB2_FIND_INVALID;
    }

    const uint8_t* p = base + 8;
    const uint8_t* end = base + total_size;

    while (p + sizeof(struct mb2_tag) <= end) {
        const struct mb2_tag* tag = (const struct mb2_tag*)p;

        if (tag->type == MB2_TAG_TYPE_END && tag->size == 8) {
            return MB2_FIND_NOT_FOUND;
        }

        if (tag->size < 8) {
            return MB2_FIND_INVALID;
        }
        if (p + tag->size > end) {
            return MB2_FIND_INVALID;
        }

        if (tag->type == want_type) {
            *out = tag;
            return MB2_FIND_OK;
        }

        p += align8_u32(tag->size);
    }

    return MB2_FIND_NOT_FOUND;
}

struct avail_region {
    uint64_t start;
    uint64_t end;
    uint64_t bytes;
};

static void avail_region_consider(struct avail_region* best, uint64_t start, uint64_t end) {
    if (!best || end <= start) {
        return;
    }

    uint64_t bytes = end - start;
    if (bytes > best->bytes) {
        best->bytes = bytes;
        best->start = start;
        best->end = end;
    }
}

static int mmap_dump_and_pick_best(const struct mb2_tag_mmap* mmap_tag, struct avail_region* best) {
    if (!mmap_tag || !best) {
        return 0;
    }

    if (mmap_tag->size < sizeof(struct mb2_tag_mmap)) {
        printk("mmap: invalid mmap tag size\n");
        return 0;
    }
    if (mmap_tag->entry_size < sizeof(struct mb2_mmap_entry)) {
        printk("mmap: invalid entry size\n");
        return 0;
    }

    const uint8_t* entries = (const uint8_t*)mmap_tag + sizeof(struct mb2_tag_mmap);
    const uint8_t* entries_end = (const uint8_t*)mmap_tag + mmap_tag->size;
    const uint64_t one_mib = 0x100000ull;

    printk("MB2 Memory Map (tag_size=%u, entry_size=%u, version=%u)\n",
        mmap_tag->size, mmap_tag->entry_size, mmap_tag->entry_version);

    best->start = 0;
    best->end = 0;
    best->bytes = 0;

    for (const uint8_t* p = entries; p + mmap_tag->entry_size <= entries_end; p += mmap_tag->entry_size) {
        const struct mb2_mmap_entry* e = (const struct mb2_mmap_entry*)p;

        uint64_t region_start = e->addr;
        uint64_t region_end = u64_add_sat(e->addr, e->len);

        /* Also show size in MB (MiB), rounded up. */
        uint32_t len_mb = (uint32_t)((e->len + ((uint64_t)1 << 20) - 1) >> 20);

        printk("  base=");
        printk_hex_u64(region_start);
        printk("  end=");
        printk_hex_u64(region_end);
        printk("  len=");
        printk_hex_u64(e->len);
        printk(" (%uMB)", len_mb);
        printk("  type=%u\n", e->type);

        if (e->type != 1u) {
            continue;
        }
        if (region_end == UINT64_MAX || region_end <= region_start) {
            continue;
        }

        /* Clamp available RAM to >= 1MiB to match the expected summary. */
        uint64_t s = (region_start < one_mib) ? one_mib : region_start;
        uint64_t t = region_end;
        if (t <= s) {
            continue;
        }

        avail_region_consider(best, s, t);
    }

    return 1;
}

static void mmap_print_summary(const struct avail_region* best) {
    if (!best || best->bytes == 0 || best->end <= best->start) {
        printk("mmap: no available RAM regions found\n");
        return;
    }

    /* Round up to MiB so 125.875MiB prints as 126MB. */
    uint32_t mb = (uint32_t)((best->bytes + ((uint64_t)1 << 20) - 1) >> 20);

    printk("Available RAM: ");
    printk_hex_range(best->start, best->end);
    printk(" (%uMB)\n", mb);
}


void mmap_print(void) {
    if (!g_mb2_info) {
        printk("mmap: no multiboot2 info\n");
        return;
    }

    const uint8_t* base = (const uint8_t*)g_mb2_info;
    uint32_t total_size = *(const uint32_t*)(base + 0);

    const struct mb2_tag* tag = NULL;
    enum mb2_find_result r = mb2_find_tag(base, total_size, MB2_TAG_TYPE_MMAP, &tag);
    if (r == MB2_FIND_NOT_FOUND) {
        printk("mmap: Multiboot2 tag type 6 (Memory Map) not found\n");
        return;
    }
    if (r != MB2_FIND_OK || !tag) {
        printk("mmap: invalid multiboot2 info\n");
        return;
    }

    struct avail_region best;
    if (!mmap_dump_and_pick_best((const struct mb2_tag_mmap*)tag, &best)) {
        return;
    }

    mmap_print_summary(&best);
}
