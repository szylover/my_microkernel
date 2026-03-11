
#include <stdint.h>
#include <stddef.h>

#include "pmm.h"

#include "printk.h"

/*
 * Physical Memory Manager (PMM) — bitmap page allocator
 *
 * [WHY]
 *   在还没有分页 (VMM) / kmalloc 之前，内核依然需要“最小单位”的物理内存：4KiB 页。
 *   PMM 在这里扮演“批发商”：只负责按页给/收，不做复杂的块管理。
 *
 * [CPU STATE]
 *   PMM 纯软件数据结构；不会修改 CR0/CR3，不会改变特权级或地址空间。
 *   目前内核处于 identity mapping 阶段（物理地址==虚拟地址）。
 */

#define PMM_PAGE_SIZE 4096u
#define PMM_ONE_MIB   0x100000u

/*
 * [ASSUMPTION]
 *   目前内核仍处于 identity mapping（物理地址 == 线性地址）。
 *   因此：PMM 返回的“物理页地址”可以直接当作指针使用。
 */

/* Multiboot2 info pointer is stored by kmain.c for later use. */
extern const void* g_mb2_info;

/* Kernel physical bounds exported by linker.ld. */
extern char __kernel_phys_start[];
extern char __kernel_phys_end[];

/* Multiboot2 tag header (common prefix for all tags). */
struct mb2_tag {
	uint32_t type;
	uint32_t size;
};

enum {
	MB2_TAG_TYPE_END  = 0,
	MB2_TAG_TYPE_MMAP = 6,
};

struct mb2_tag_mmap {
	uint32_t type;
	uint32_t size;
	uint32_t entry_size;
	uint32_t entry_version;
} __attribute__((packed));

struct mb2_mmap_entry {
	uint64_t addr;
	uint64_t len;
	uint32_t type;
	uint32_t reserved;
} __attribute__((packed));

/* ================================
 * Small integer helpers
 * ================================ */
static uint32_t align8_u32(uint32_t n) { return (n + 7u) & ~7u; }
static uint32_t align_up_u32(uint32_t v, uint32_t a) { return (v + (a - 1u)) & ~(a - 1u); }
static uint32_t align_down_u32(uint32_t v, uint32_t a) { return v & ~(a - 1u); }

static uint32_t u32_max(uint32_t a, uint32_t b) { return (a > b) ? a : b; }

/* Saturating add for (addr + len) parsing; avoids overflow UB/bugs. */
static uint64_t u64_add_sat(uint64_t a, uint64_t b) {
	uint64_t r = a + b;
	if (r < a) {
		return UINT64_MAX;
	}
	return r;
}

/* Tiny memset replacement (freestanding kernel; avoid libc dependency). */
static void memfill_u8(uint8_t* dst, uint8_t value, uint32_t bytes) {
	for (uint32_t i = 0; i < bytes; i++) {
		dst[i] = value;
	}
}

enum mb2_find_result {
	MB2_FIND_OK = 0,
	MB2_FIND_NOT_FOUND,
	MB2_FIND_INVALID,
};

/*
 * Find the first Multiboot2 tag with wanted type.
 * Returns MB2_FIND_OK and sets *out on success.
 */
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

/* ================================
 * Bitmap internals
 *   bit=1 => used/reserved
 *   bit=0 => free
 * ================================ */
static uint8_t* g_pmm_bitmap = NULL;
static uint32_t g_pmm_bitmap_bytes = 0;      /* ceil(pages/8) */
static uint32_t g_pmm_bitmap_storage_bytes = 0; /* mapped/allocated bytes (page aligned) */

static uint32_t g_pmm_base = 0;              /* physical address of page index 0 */
static uint32_t g_pmm_pages = 0;             /* total pages tracked by bitmap */
static uint32_t g_pmm_free = 0;              /* cached free page count */
static int g_pmm_ready = 0;

static uint32_t pmm_managed_end(void) {
	return g_pmm_base + g_pmm_pages * PMM_PAGE_SIZE;
}

static int pmm_addr_is_page_aligned(uint32_t addr) {
	return (addr & (PMM_PAGE_SIZE - 1u)) == 0;
}

static int pmm_addr_in_managed_range(uint32_t addr) {
	return (addr >= g_pmm_base) && (addr < pmm_managed_end());
}

static uint32_t pmm_addr_to_index(uint32_t addr) {
	return (addr - g_pmm_base) / PMM_PAGE_SIZE;
}

static uint32_t pmm_index_to_addr(uint32_t idx) {
	return g_pmm_base + idx * PMM_PAGE_SIZE;
}

static int bitmap_test(uint32_t bit) {
	uint32_t byte = bit >> 3;
	uint32_t mask = 1u << (bit & 7u);
	return (g_pmm_bitmap[byte] & mask) != 0;
}

static void bitmap_set(uint32_t bit) {
	uint32_t byte = bit >> 3;
	uint32_t mask = 1u << (bit & 7u);
	g_pmm_bitmap[byte] = (uint8_t)(g_pmm_bitmap[byte] | mask);
}

static void bitmap_clear(uint32_t bit) {
	uint32_t byte = bit >> 3;
	uint32_t mask = 1u << (bit & 7u);
	g_pmm_bitmap[byte] = (uint8_t)(g_pmm_bitmap[byte] & (uint8_t)~mask);
}

/*
 * Mark a single page "used" by its physical address.
 * Note: this is used during init-time reservation, so it tolerates unaligned
 * input by aligning down.
 */
static void bitmap_mark_used_by_addr(uint32_t phys_addr) {
	if (!g_pmm_ready) {
		return;
	}
	if (!pmm_addr_is_page_aligned(phys_addr)) {
		phys_addr = align_down_u32(phys_addr, PMM_PAGE_SIZE);
	}
	if (!pmm_addr_in_managed_range(phys_addr)) {
		return;
	}
	uint32_t idx = pmm_addr_to_index(phys_addr);
	if (idx >= g_pmm_pages) {
		return;
	}
	if (!bitmap_test(idx)) {
		bitmap_set(idx);
		if (g_pmm_free > 0) {
			g_pmm_free--;
		}
	}
}

static void bitmap_mark_used_range(uint32_t start, uint32_t end_excl) {
	if (!g_pmm_ready) {
		return;
	}
	if (end_excl <= start) {
		return;
	}

	uint32_t s = align_down_u32(start, PMM_PAGE_SIZE);
	uint32_t e = align_up_u32(end_excl, PMM_PAGE_SIZE);
	for (uint32_t p = s; p < e; p += PMM_PAGE_SIZE) {
		bitmap_mark_used_by_addr(p);
	}
}

/* ================================
 * Multiboot2 mmap -> best usable region
 * ================================ */
/* Find the largest available RAM region (type=1), clamped to >=1MiB and <4GiB. */
static int pmm_pick_best_region(const struct mb2_tag_mmap* mmap_tag, uint32_t* out_start, uint32_t* out_end) {
	if (!mmap_tag || !out_start || !out_end) {
		return 0;
	}

	if (mmap_tag->size < sizeof(struct mb2_tag_mmap)) {
		return 0;
	}
	if (mmap_tag->entry_size < sizeof(struct mb2_mmap_entry)) {
		return 0;
	}

	const uint8_t* entries = (const uint8_t*)mmap_tag + sizeof(struct mb2_tag_mmap);
	const uint8_t* entries_end = (const uint8_t*)mmap_tag + mmap_tag->size;

	uint64_t best_bytes = 0;
	uint32_t best_s = 0;
	uint32_t best_e = 0;

	for (const uint8_t* p = entries; p + mmap_tag->entry_size <= entries_end; p += mmap_tag->entry_size) {
		const struct mb2_mmap_entry* e = (const struct mb2_mmap_entry*)p;
		if (e->type != 1u) {
			continue;
		}

		uint64_t region_start64 = e->addr;
		uint64_t region_end64 = u64_add_sat(e->addr, e->len);
		if (region_end64 == UINT64_MAX || region_end64 <= region_start64) {
			continue;
		}

		/* Clamp to 32-bit addressable space and skip >4GiB-only regions. */
		if (region_start64 >= 0x100000000ull) {
			continue;
		}
		uint32_t region_start = (uint32_t)region_start64;
		uint32_t region_end = (region_end64 >= 0x100000000ull) ? 0xffffffffu : (uint32_t)region_end64;

		/* Clamp usable start to >= 1MiB, per stage-1 mmap summary convention. */
		if (region_end <= PMM_ONE_MIB) {
			continue;
		}
		if (region_start < PMM_ONE_MIB) {
			region_start = PMM_ONE_MIB;
		}
		if (region_end <= region_start) {
			continue;
		}

		uint64_t bytes = (uint64_t)(region_end - region_start);
		if (bytes > best_bytes) {
			best_bytes = bytes;
			best_s = region_start;
			best_e = region_end;
		}
	}

	if (best_bytes == 0) {
		return 0;
	}

	*out_start = best_s;
	*out_end = best_e;
	return 1;
}

struct pmm_layout {
	uint32_t best_start;
	uint32_t best_end;
	uint32_t managed_start;
	uint32_t managed_end;
	uint32_t pages;
	uint32_t bitmap_bytes;
	uint32_t bitmap_storage_bytes;
	uint32_t bitmap_pages;
};

static void pmm_reset_state(void) {
	g_pmm_ready = 0;
	g_pmm_bitmap = NULL;
	g_pmm_bitmap_bytes = 0;
	g_pmm_bitmap_storage_bytes = 0;
	g_pmm_base = 0;
	g_pmm_pages = 0;
	g_pmm_free = 0;
}

static int pmm_compute_layout(const struct mb2_tag_mmap* mmap_tag, struct pmm_layout* out) {
	if (!mmap_tag || !out) {
		return 0;
	}

	uint32_t best_start = 0;
	uint32_t best_end = 0;
	if (!pmm_pick_best_region(mmap_tag, &best_start, &best_end)) {
		return 0;
	}

	/*
	 * Reserve everything below:
	 * - 1MiB (already clamped by picker)
	 * - kernel image (.text/.rodata/.data/.bss + stack)
	 */
	uint32_t kernel_end = (uint32_t)(uintptr_t)__kernel_phys_end;
	uint32_t managed_start = u32_max(best_start, align_up_u32(kernel_end, PMM_PAGE_SIZE));
	uint32_t managed_end = best_end;

	managed_start = align_up_u32(managed_start, PMM_PAGE_SIZE);
	managed_end = align_down_u32(managed_end, PMM_PAGE_SIZE);
	if (managed_end <= managed_start + PMM_PAGE_SIZE) {
		return 0;
	}

	uint32_t bytes = managed_end - managed_start;
	uint32_t pages = bytes / PMM_PAGE_SIZE;
	if (pages < 8) {
		return 0;
	}

	uint32_t bitmap_bytes = (pages + 7u) / 8u;
	uint32_t bitmap_storage = align_up_u32(bitmap_bytes, PMM_PAGE_SIZE);
	uint32_t bitmap_pages = bitmap_storage / PMM_PAGE_SIZE;
	if (bitmap_pages >= pages) {
		return 0;
	}

	out->best_start = best_start;
	out->best_end = best_end;
	out->managed_start = managed_start;
	out->managed_end = managed_end;
	out->pages = pages;
	out->bitmap_bytes = bitmap_bytes;
	out->bitmap_storage_bytes = bitmap_storage;
	out->bitmap_pages = bitmap_pages;
	return 1;
}

static void pmm_bitmap_init_and_free_tail(const struct pmm_layout* layout) {
	/*
	 * Bitmap storage lives at the beginning of managed region.
	 * Start with all-ones (everything used), then clear bits for free pages.
	 */
	memfill_u8(g_pmm_bitmap, 0xffu, layout->bitmap_storage_bytes);

	g_pmm_free = 0;
	for (uint32_t i = layout->bitmap_pages; i < g_pmm_pages; i++) {
		bitmap_clear(i);
		g_pmm_free++;
	}
}

static void pmm_reserve_multiboot2_info(void) {
	/*
	 * Reserve Multiboot2 info structure so shell commands (e.g. mmap) stay valid.
	 *
	 * [WHY]
	 *   g_mb2_info 指向 GRUB 留下的 tag 列表；如果我们把它所在页分配出去并写坏，
	 *   后续 `mmap` 命令会解析到垃圾数据甚至崩溃。
	 */
	uint32_t mb2_phys = (uint32_t)(uintptr_t)g_mb2_info;
	uint32_t mb2_size = *(const uint32_t*)((const uint8_t*)g_mb2_info + 0);
	bitmap_mark_used_range(mb2_phys, mb2_phys + mb2_size);
}

static void pmm_print_summary(const struct pmm_layout* layout) {
	printk("[pmm] best region: 0x%08x - 0x%08x\n", layout->best_start, layout->best_end);
	printk("[pmm] managed    : 0x%08x - 0x%08x (%u pages)\n", layout->managed_start, layout->managed_end, g_pmm_pages);
	printk("[pmm] bitmap @ 0x%08x (%u bytes, %u pages)\n", (uint32_t)(uintptr_t)g_pmm_bitmap, g_pmm_bitmap_bytes, layout->bitmap_pages);
	printk("[pmm] free pages : %u\n", g_pmm_free);
}

static void pmm_resync_free_count(void) {
	/*
	 * Recompute g_pmm_free by scanning the bitmap.
	 *
	 * [WHY]
	 *   g_pmm_free 是缓存值；在早期阶段我们宁可偶尔 O(n) 纠正它，
	 *   也不引入更复杂的数据结构。
	 */
	g_pmm_free = 0;
	for (uint32_t i = 0; i < g_pmm_pages; i++) {
		if (!bitmap_test(i)) {
			g_pmm_free++;
		}
	}
}

static int pmm_find_free_index(uint32_t* out_idx) {
	/*
	 * Find a free page index by scanning the bitmap.
	 * Returns 1 on success, 0 if none found.
	 */
	if (!out_idx) {
		return 0;
	}

	for (uint32_t byte = 0; byte < g_pmm_bitmap_bytes; byte++) {
		uint8_t v = g_pmm_bitmap[byte];
		if (v == 0xffu) {
			continue;
		}
		for (uint32_t bit = 0; bit < 8; bit++) {
			uint32_t idx = (byte << 3) + bit;
			if (idx >= g_pmm_pages) {
				break;
			}
			if (!bitmap_test(idx)) {
				*out_idx = idx;
				return 1;
			}
		}
	}

	return 0;
}

void pmm_init(void) {
	/*
	 * Initialize the bitmap allocator from Multiboot2 mmap.
	 *
	 * [INPUT]
	 *   g_mb2_info: Multiboot2 info pointer saved by kmain.
	 * [OUTPUT]
	 *   After success: pmm_alloc_page/pmm_free_page become usable.
	 */
	pmm_reset_state();

	if (!g_mb2_info) {
		printk("[pmm] no multiboot2 info; PMM disabled\n");
		return;
	}

	const uint8_t* base = (const uint8_t*)g_mb2_info;
	uint32_t total_size = *(const uint32_t*)(base + 0);

	const struct mb2_tag* tag = NULL;
	enum mb2_find_result r = mb2_find_tag(base, total_size, MB2_TAG_TYPE_MMAP, &tag);
	if (r != MB2_FIND_OK || !tag) {
		printk("[pmm] multiboot2 mmap tag not found; PMM disabled\n");
		return;
	}

	struct pmm_layout layout;
	if (!pmm_compute_layout((const struct mb2_tag_mmap*)tag, &layout)) {
		printk("[pmm] cannot compute PMM layout (no usable region / too small)\n");
		return;
	}

	g_pmm_base = layout.managed_start;
	g_pmm_pages = layout.pages;
	g_pmm_bitmap = (uint8_t*)(uintptr_t)layout.managed_start;
	g_pmm_bitmap_bytes = layout.bitmap_bytes;
	g_pmm_bitmap_storage_bytes = layout.bitmap_storage_bytes;
	g_pmm_ready = 1;

	pmm_bitmap_init_and_free_tail(&layout);
	pmm_reserve_multiboot2_info();
	pmm_print_summary(&layout);
}

void* pmm_alloc_page(void) {
	/*
	 * Allocate one 4KiB page.
	 *
	 * [NOTE]
	 *   Early-kernel implementation uses a linear scan over the bitmap.
	 */
	if (!g_pmm_ready || !g_pmm_bitmap || g_pmm_pages == 0) {
		return NULL;
	}
	if (g_pmm_free == 0) {
		return NULL;
	}

	uint32_t idx = 0;
	if (pmm_find_free_index(&idx)) {
		bitmap_set(idx);
		g_pmm_free--;
		return (void*)(uintptr_t)pmm_index_to_addr(idx);
	}

	/* Bitmap says free>0 but we didn't find any. Re-sync count. */
	pmm_resync_free_count();
	return NULL;
}

void pmm_free_page(void* page) {
	/*
	 * Free a page previously returned by pmm_alloc_page().
	 *
	 * [SAFETY]
	 *   - Must be page-aligned
	 *   - Must fall into managed range
	 *   - Detect double-free
	 */
	if (!g_pmm_ready || !page) {
		return;
	}

	uint32_t addr = (uint32_t)(uintptr_t)page;
	if (!pmm_addr_is_page_aligned(addr)) {
		printk("[pmm] free: unaligned addr=0x%08x\n", addr);
		return;
	}
	if (!pmm_addr_in_managed_range(addr)) {
		printk("[pmm] free: out of range addr=0x%08x\n", addr);
		return;
	}

	uint32_t idx = pmm_addr_to_index(addr);
	if (idx >= g_pmm_pages) {
		return;
	}
	if (!bitmap_test(idx)) {
		printk("[pmm] free: double free addr=0x%08x\n", addr);
		return;
	}
	bitmap_clear(idx);
	g_pmm_free++;
}

unsigned pmm_total_pages(void) {
	return g_pmm_ready ? (unsigned)g_pmm_pages : 0u;
}

unsigned pmm_free_pages(void) {
	return g_pmm_ready ? (unsigned)g_pmm_free : 0u;
}

unsigned pmm_managed_base(void) {
	return g_pmm_ready ? (unsigned)g_pmm_base : 0u;
}

int pmm_page_is_used(unsigned page_index) {
	if (!g_pmm_ready) {
		return -1;
	}
	if (page_index >= g_pmm_pages) {
		return -1;
	}
	return bitmap_test((uint32_t)page_index) ? 1 : 0;
}

