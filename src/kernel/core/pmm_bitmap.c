
#include <stdint.h>
#include <stddef.h>

#include "pmm.h"
#include "vmm.h"

#include "printk.h"

/*
 * Physical Memory Manager (PMM) — bitmap backend
 *
 * [WHY]
 *   bitmap 是最简单的页分配算法：每个物理页对应 1 bit（0=free, 1=used）。
 *   优点：实现简单、查询状态 O(1)。
 *   缺点：分配需要线性扫描 O(n)、无法高效分配连续多页。
 *
 *   本文件是 pmm_ops_t 的第一个后端实现。
 *   通过 pmm_bitmap_get_ops() 导出操作表，由 pmm.c dispatch 层调用。
 *
 * [CPU STATE]
 *   PMM 纯软件数据结构；不会修改 CR0/CR3，不会改变特权级或地址空间。
 */

#define PMM_PAGE_SIZE 4096u
#define PMM_ONE_MIB   0x100000u

/* Maximum number of usable RAM regions PMM will manage (fixed, no kmalloc). */
#define PMM_MAX_REGIONS 32u

/*
 * [NOTE]
 *   PMM 返回的是物理地址。调用者需要通过 PHYS_TO_VIRT() 转换为虚拟地址
 *   才能解引用。PMM 内部的 bitmap 指针也使用虚拟地址存储。
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
static uint32_t u32_min(uint32_t a, uint32_t b) { return (a < b) ? a : b; }

static int ranges_overlap_u32(uint32_t a_start, uint32_t a_end, uint32_t b_start, uint32_t b_end) {
	uint32_t s = u32_max(a_start, b_start);
	uint32_t e = u32_min(a_end, b_end);
	return e > s;
}

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
 * Bitmap internals (per-region)
 *   bit=1 => used/reserved
 *   bit=0 => free
 * ================================ */

struct pmm_region {
	uint32_t base; /* physical address of page index 0 (for this region) */
	uint32_t pages;
	uint8_t* bitmap;
	uint32_t bitmap_bytes; /* ceil(pages/8) */
	uint32_t bitmap_storage_bytes; /* page-aligned storage */
	uint32_t bitmap_pages; /* bitmap storage in pages */
	uint32_t free;
};

static struct pmm_region g_regions[PMM_MAX_REGIONS];
static uint32_t g_region_count = 0;
static uint32_t g_region_rr = 0; /* round-robin start index for alloc */

static uint32_t g_pmm_total_pages = 0;
static uint32_t g_pmm_total_free = 0;
static int g_pmm_ready = 0;

static int pmm_addr_is_page_aligned(uint32_t addr) {
	return (addr & (PMM_PAGE_SIZE - 1u)) == 0;
}

static int region_contains_addr(const struct pmm_region* r, uint32_t addr) {
	if (!r || r->pages == 0) return 0;
	uint32_t end = r->base + r->pages * PMM_PAGE_SIZE;
	return addr >= r->base && addr < end;
}

static uint32_t region_addr_to_index(const struct pmm_region* r, uint32_t addr) {
	return (addr - r->base) / PMM_PAGE_SIZE;
}

static uint32_t region_index_to_addr(const struct pmm_region* r, uint32_t idx) {
	return r->base + idx * PMM_PAGE_SIZE;
}

static int bitmap_test_r(const struct pmm_region* r, uint32_t bit) {
	uint32_t byte = bit >> 3;
	uint32_t mask = 1u << (bit & 7u);
	return (r->bitmap[byte] & mask) != 0;
}

static void bitmap_set_r(struct pmm_region* r, uint32_t bit) {
	uint32_t byte = bit >> 3;
	uint32_t mask = 1u << (bit & 7u);
	r->bitmap[byte] = (uint8_t)(r->bitmap[byte] | mask);
}

static void bitmap_clear_r(struct pmm_region* r, uint32_t bit) {
	uint32_t byte = bit >> 3;
	uint32_t mask = 1u << (bit & 7u);
	r->bitmap[byte] = (uint8_t)(r->bitmap[byte] & (uint8_t)~mask);
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

	for (uint32_t ri = 0; ri < g_region_count; ri++) {
		struct pmm_region* r = &g_regions[ri];
		if (!region_contains_addr(r, phys_addr)) {
			continue;
		}
		uint32_t idx = region_addr_to_index(r, phys_addr);
		if (idx >= r->pages) {
			return;
		}
		if (!bitmap_test_r(r, idx)) {
			bitmap_set_r(r, idx);
			if (r->free > 0) {
				r->free--;
				if (g_pmm_total_free > 0) {
					g_pmm_total_free--;
				}
			}
		}
		return;
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

static void pmm_reset_state(void) {
	g_pmm_ready = 0;
	g_region_count = 0;
	g_region_rr = 0;
	g_pmm_total_pages = 0;
	g_pmm_total_free = 0;
	for (uint32_t i = 0; i < PMM_MAX_REGIONS; i++) {
		g_regions[i].base = 0;
		g_regions[i].pages = 0;
		g_regions[i].bitmap = NULL;
		g_regions[i].bitmap_bytes = 0;
		g_regions[i].bitmap_storage_bytes = 0;
		g_regions[i].bitmap_pages = 0;
		g_regions[i].free = 0;
	}
}

struct pmm_region_layout {
	uint32_t usable_start;
	uint32_t usable_end;
	uint32_t managed_start;
	uint32_t managed_end;
	uint32_t pages;
	uint32_t bitmap_bytes;
	uint32_t bitmap_storage_bytes;
	uint32_t bitmap_pages;
};

static int pmm_compute_region_layout(
	uint32_t usable_start,
	uint32_t usable_end,
	uint32_t kernel_start,
	uint32_t kernel_end,
	uint32_t mb2_start,
	uint32_t mb2_end,
	struct pmm_region_layout* out
) {
	if (!out) return 0;
	if (usable_end <= usable_start) return 0;

	uint32_t managed_start = usable_start;
	uint32_t managed_end = usable_end;

	/*
	 * We store the bitmap at the beginning of the managed range.
	 * So we must ensure that "managed_start .. managed_start+bitmap_storage" does
	 * not overlap critical boot-time data (kernel image, multiboot2 info).
	 */
	for (unsigned attempt = 0; attempt < 4; attempt++) {
		/* If the kernel image overlaps this usable range, skip kernel pages. */
		if (ranges_overlap_u32(managed_start, managed_end, kernel_start, kernel_end)) {
			if (managed_start < kernel_end && managed_end > kernel_start) {
				managed_start = u32_max(managed_start, align_up_u32(kernel_end, PMM_PAGE_SIZE));
			}
		}

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

		uint32_t bitmap_start = managed_start;
		uint32_t bitmap_end = bitmap_start + bitmap_storage;

		/* Avoid corrupting Multiboot2 info while we are still parsing it. */
		if (mb2_end > mb2_start && ranges_overlap_u32(bitmap_start, bitmap_end, mb2_start, mb2_end)) {
			managed_start = align_up_u32(mb2_end, PMM_PAGE_SIZE);
			continue;
		}

		out->usable_start = usable_start;
		out->usable_end = usable_end;
		out->managed_start = managed_start;
		out->managed_end = managed_end;
		out->pages = pages;
		out->bitmap_bytes = bitmap_bytes;
		out->bitmap_storage_bytes = bitmap_storage;
		out->bitmap_pages = bitmap_pages;
		return 1;
	}

	return 0;
}

static void pmm_region_bitmap_init_and_free_tail(struct pmm_region* r) {
	/*
	 * Bitmap storage lives at the beginning of this managed region.
	 * Start with all-ones (everything used), then clear bits for free pages.
	 */
	memfill_u8(r->bitmap, 0xffu, r->bitmap_storage_bytes);

	r->free = 0;
	for (uint32_t i = r->bitmap_pages; i < r->pages; i++) {
		bitmap_clear_r(r, i);
		r->free++;
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
	const uint8_t* mb2_virt = (const uint8_t*)PHYS_TO_VIRT(mb2_phys);
	uint32_t mb2_size = *(const uint32_t*)(mb2_virt + 0);
	bitmap_mark_used_range(mb2_phys, mb2_phys + mb2_size);
}

static void pmm_reserve_kernel_image(void) {
	/* Reserve the kernel image pages so PMM never hands them out. */
	uint32_t k_start = (uint32_t)(uintptr_t)__kernel_phys_start;
	uint32_t k_end = (uint32_t)(uintptr_t)__kernel_phys_end;
	if (k_end > k_start) {
		bitmap_mark_used_range(k_start, k_end);
	}
}

static void pmm_print_summary(void) {
	printk("[pmm] regions    : %u\n", (unsigned)g_region_count);
	for (uint32_t i = 0; i < g_region_count; i++) {
		const struct pmm_region* r = &g_regions[i];
		uint32_t end = r->base + r->pages * PMM_PAGE_SIZE;
		printk("[pmm] r%u managed : 0x%08x - 0x%08x (%u pages)\n", (unsigned)i, r->base, end, (unsigned)r->pages);
		printk("[pmm] r%u bitmap  : 0x%08x (%u bytes, %u pages)\n", (unsigned)i, (uint32_t)(uintptr_t)r->bitmap, (unsigned)r->bitmap_bytes, (unsigned)r->bitmap_pages);
		printk("[pmm] r%u free    : %u\n", (unsigned)i, (unsigned)r->free);
	}
	printk("[pmm] total pages: %u\n", (unsigned)g_pmm_total_pages);
	printk("[pmm] free pages : %u\n", (unsigned)g_pmm_total_free);
}

static void pmm_resync_free_count(void) {
	/*
	 * Recompute g_pmm_free by scanning the bitmap.
	 *
	 * [WHY]
	 *   g_pmm_free 是缓存值；在早期阶段我们宁可偶尔 O(n) 纠正它，
	 *   也不引入更复杂的数据结构。
	 */
	g_pmm_total_free = 0;
	for (uint32_t ri = 0; ri < g_region_count; ri++) {
		struct pmm_region* r = &g_regions[ri];
		r->free = 0;
		for (uint32_t i = 0; i < r->pages; i++) {
			if (!bitmap_test_r(r, i)) {
				r->free++;
			}
		}
		g_pmm_total_free += r->free;
	}
}

static int pmm_find_free_index(const struct pmm_region* r, uint32_t* out_idx) {
	/*
	 * Find a free page index by scanning the bitmap.
	 * Returns 1 on success, 0 if none found.
	 */
	if (!r || !out_idx) {
		return 0;
	}

	for (uint32_t byte = 0; byte < r->bitmap_bytes; byte++) {
		uint8_t v = r->bitmap[byte];
		if (v == 0xffu) {
			continue;
		}
		for (uint32_t bit = 0; bit < 8; bit++) {
			uint32_t idx = (byte << 3) + bit;
			if (idx >= r->pages) {
				break;
			}
			if (!bitmap_test_r(r, idx)) {
				*out_idx = idx;
				return 1;
			}
		}
	}

	return 0;
}

static void pmm_selftest(void) {
	/*
	 * Minimal PMM self-test.
	 *
	 * Goals:
	 *  - Validate alloc/free basic correctness
	 *  - Validate the returned memory is writable/readable (identity mapping stage)
	 *  - Validate free-page counter bookkeeping restores after frees
	 *
	 * Non-goals:
	 *  - Exhaustive testing or performance benchmarking
	 */
	if (!g_pmm_ready || g_region_count == 0 || g_pmm_total_pages == 0) {
		return;
	}

	unsigned free_before = (unsigned)g_pmm_total_free;
	if (free_before < 4u) {
		printk("[pmm] selftest: skip (free=%u)\n", free_before);
		return;
	}

	/* Keep it small so it is fast and low-risk during boot. */
	enum { TEST_PAGES = 4 };
	void* pages[TEST_PAGES];
	for (unsigned i = 0; i < TEST_PAGES; i++) {
		pages[i] = NULL;
	}

	unsigned allocated = 0;
	for (unsigned i = 0; i < TEST_PAGES; i++) {
		void* p = pmm_alloc_page();
		if (!p) {
			break;
		}
		pages[i] = p;
		allocated++;
	}

	if (allocated != TEST_PAGES) {
		for (unsigned i = 0; i < TEST_PAGES; i++) {
			if (pages[i]) {
				pmm_free_page(pages[i]);
			}
		}
		printk("[pmm] selftest: FAIL (alloc %u/%u)\n", allocated, (unsigned)TEST_PAGES);
		return;
	}

	if ((unsigned)g_pmm_total_free != free_before - TEST_PAGES) {
		for (unsigned i = 0; i < TEST_PAGES; i++) {
			pmm_free_page(pages[i]);
		}
		printk("[pmm] selftest: FAIL (free counter mismatch after alloc)\n");
		return;
	}

	/* Write + verify a simple per-page pattern (convert phys→virt). */
	for (unsigned i = 0; i < TEST_PAGES; i++) {
		uint32_t* w = (uint32_t*)PHYS_TO_VIRT((uint32_t)(uintptr_t)pages[i]);
		uint32_t pattern = 0xC0FFEE00u ^ (i * 0x11111111u);
		for (unsigned j = 0; j < (PMM_PAGE_SIZE / 4u); j++) {
			w[j] = pattern;
		}
		for (unsigned j = 0; j < (PMM_PAGE_SIZE / 4u); j++) {
			if (w[j] != pattern) {
				for (unsigned k = 0; k < TEST_PAGES; k++) {
					pmm_free_page(pages[k]);
				}
				printk("[pmm] selftest: FAIL (pattern verify)\n");
				return;
			}
		}
	}

	for (unsigned i = 0; i < TEST_PAGES; i++) {
		pmm_free_page(pages[i]);
	}

	if ((unsigned)g_pmm_total_free != free_before) {
		printk("[pmm] selftest: FAIL (free counter mismatch after free)\n");
		return;
	}

	printk("[pmm] selftest: OK (%u pages)\n", (unsigned)TEST_PAGES);
}

static void pmm_recompute_totals(void) {
	g_pmm_total_pages = 0;
	g_pmm_total_free = 0;
	for (uint32_t i = 0; i < g_region_count; i++) {
		g_pmm_total_pages += g_regions[i].pages;
		g_pmm_total_free += g_regions[i].free;
	}
}

static int pmm_add_region(
	uint32_t usable_start,
	uint32_t usable_end,
	uint32_t kernel_start,
	uint32_t kernel_end,
	uint32_t mb2_start,
	uint32_t mb2_end
) {
	if (g_region_count >= PMM_MAX_REGIONS) {
		return 0;
	}

	struct pmm_region_layout layout;
	if (!pmm_compute_region_layout(usable_start, usable_end, kernel_start, kernel_end, mb2_start, mb2_end, &layout)) {
		return 0;
	}

	struct pmm_region* r = &g_regions[g_region_count];
	r->base = layout.managed_start;
	r->pages = layout.pages;
	r->bitmap = (uint8_t*)PHYS_TO_VIRT(layout.managed_start);
	r->bitmap_bytes = layout.bitmap_bytes;
	r->bitmap_storage_bytes = layout.bitmap_storage_bytes;
	r->bitmap_pages = layout.bitmap_pages;
	r->free = 0;

	pmm_region_bitmap_init_and_free_tail(r);
	g_region_count++;
	return 1;
}

void pmm_bitmap_init(void) {
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

	const uint8_t* base = (const uint8_t*)PHYS_TO_VIRT((uint32_t)(uintptr_t)g_mb2_info);
	uint32_t total_size = *(const uint32_t*)(base + 0);

	const struct mb2_tag* tag = NULL;
	enum mb2_find_result r = mb2_find_tag(base, total_size, MB2_TAG_TYPE_MMAP, &tag);
	if (r != MB2_FIND_OK || !tag) {
		printk("[pmm] multiboot2 mmap tag not found; PMM disabled\n");
		return;
	}

	/* Build PMM regions from all usable mmap entries (type=1). */
	const struct mb2_tag_mmap* mmap_tag = (const struct mb2_tag_mmap*)tag;
	if (mmap_tag->size < sizeof(struct mb2_tag_mmap) || mmap_tag->entry_size < sizeof(struct mb2_mmap_entry)) {
		printk("[pmm] invalid multiboot2 mmap tag; PMM disabled\n");
		return;
	}

	const uint8_t* entries = (const uint8_t*)mmap_tag + sizeof(struct mb2_tag_mmap);
	const uint8_t* entries_end = (const uint8_t*)mmap_tag + mmap_tag->size;
	uint32_t kernel_start = (uint32_t)(uintptr_t)__kernel_phys_start;
	uint32_t kernel_end = (uint32_t)(uintptr_t)__kernel_phys_end;
	uint32_t mb2_start = (uint32_t)(uintptr_t)g_mb2_info;
	uint32_t mb2_end = mb2_start + *(const uint32_t*)((const uint8_t*)PHYS_TO_VIRT(mb2_start) + 0);

	uint32_t added = 0;
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

		if (region_start64 >= 0x100000000ull) {
			continue;
		}
		uint32_t region_start = (uint32_t)region_start64;
		uint32_t region_end = (region_end64 >= 0x100000000ull) ? 0xffffffffu : (uint32_t)region_end64;

		/* Clamp usable start to >= 1MiB. */
		if (region_end <= PMM_ONE_MIB) {
			continue;
		}
		if (region_start < PMM_ONE_MIB) {
			region_start = PMM_ONE_MIB;
		}
		if (region_end <= region_start) {
			continue;
		}

		if (!pmm_add_region(region_start, region_end, kernel_start, kernel_end, mb2_start, mb2_end)) {
			if (g_region_count >= PMM_MAX_REGIONS) {
				printk("[pmm] WARN: region cap reached (%u), ignore remaining mmap\n", (unsigned)PMM_MAX_REGIONS);
				break;
			}
			continue;
		}
		added++;
	}

	if (added == 0 || g_region_count == 0) {
		printk("[pmm] cannot build PMM regions (no usable region / too small)\n");
		return;
	}

	g_pmm_ready = 1;
	pmm_recompute_totals();
	pmm_reserve_kernel_image();
	pmm_reserve_multiboot2_info();
	pmm_resync_free_count();
	pmm_selftest();
	pmm_print_summary();
}

void* pmm_bitmap_alloc_page(void) {
	/*
	 * Allocate one 4KiB page.
	 *
	 * [NOTE]
	 *   Early-kernel implementation uses a linear scan over the bitmap.
	 */
	if (!g_pmm_ready || g_region_count == 0 || g_pmm_total_pages == 0) {
		return NULL;
	}
	if (g_pmm_total_free == 0) {
		return NULL;
	}

	for (uint32_t off = 0; off < g_region_count; off++) {
		uint32_t ri = (g_region_rr + off) % g_region_count;
		struct pmm_region* r = &g_regions[ri];
		if (r->free == 0) {
			continue;
		}
		uint32_t idx = 0;
		if (pmm_find_free_index(r, &idx)) {
			bitmap_set_r(r, idx);
			r->free--;
			if (g_pmm_total_free > 0) {
				g_pmm_total_free--;
			}
			g_region_rr = ri;
			return (void*)(uintptr_t)region_index_to_addr(r, idx);
		}
	}

	/* Cached free count says free>0 but we didn't find any. Re-sync. */
	pmm_resync_free_count();
	return NULL;
}

void pmm_bitmap_free_page(void* page) {
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
	for (uint32_t ri = 0; ri < g_region_count; ri++) {
		struct pmm_region* r = &g_regions[ri];
		if (!region_contains_addr(r, addr)) {
			continue;
		}
		uint32_t idx = region_addr_to_index(r, addr);
		if (idx >= r->pages) {
			return;
		}
		if (!bitmap_test_r(r, idx)) {
			printk("[pmm] free: double free addr=0x%08x\n", addr);
			return;
		}
		bitmap_clear_r(r, idx);
		r->free++;
		g_pmm_total_free++;
		return;
	}

	printk("[pmm] free: out of range addr=0x%08x\n", addr);
}

unsigned pmm_bitmap_total_pages(void) {
	return g_pmm_ready ? (unsigned)g_pmm_total_pages : 0u;
}

unsigned pmm_bitmap_free_pages(void) {
	return g_pmm_ready ? (unsigned)g_pmm_total_free : 0u;
}

unsigned pmm_bitmap_managed_base(void) {
	if (!g_pmm_ready || g_region_count == 0) {
		return 0u;
	}
	uint32_t min_base = g_regions[0].base;
	for (uint32_t i = 1; i < g_region_count; i++) {
		if (g_regions[i].base != 0 && g_regions[i].base < min_base) {
			min_base = g_regions[i].base;
		}
	}
	return (unsigned)min_base;
}

unsigned pmm_bitmap_page_addr(unsigned page_index) {
	if (!g_pmm_ready) {
		return 0u;
	}
	uint32_t idx = (uint32_t)page_index;
	uint32_t acc = 0;
	for (uint32_t ri = 0; ri < g_region_count; ri++) {
		const struct pmm_region* r = &g_regions[ri];
		if (idx < acc + r->pages) {
			uint32_t local = idx - acc;
			return (unsigned)region_index_to_addr(r, local);
		}
		acc += r->pages;
	}
	return 0u;
}

int pmm_bitmap_page_is_used(unsigned page_index) {
	if (!g_pmm_ready) {
		return -1;
	}
	uint32_t idx = (uint32_t)page_index;
	uint32_t acc = 0;
	for (uint32_t ri = 0; ri < g_region_count; ri++) {
		const struct pmm_region* r = &g_regions[ri];
		if (idx < acc + r->pages) {
			uint32_t local = idx - acc;
			return bitmap_test_r(r, local) ? 1 : 0;
		}
		acc += r->pages;
	}
	return -1;
}

/* ============================================================================
 * pmm_ops_t — bitmap 后端操作表
 * ============================================================================
 *
 * [WHY] 将所有 bitmap_* 函数打包成一个 pmm_ops_t，
 *   由 pmm.c dispatch 层通过函数指针调用。
 *   新后端只需依样写一个 ops 表并注册即可替换。
 */
static const pmm_ops_t g_bitmap_ops = {
	.name         = "bitmap",
	.init         = pmm_bitmap_init,
	.alloc_page   = pmm_bitmap_alloc_page,
	.free_page    = pmm_bitmap_free_page,
	.total_pages  = pmm_bitmap_total_pages,
	.free_pages   = pmm_bitmap_free_pages,
	.managed_base = pmm_bitmap_managed_base,
	.page_addr    = pmm_bitmap_page_addr,
	.page_is_used = pmm_bitmap_page_is_used,
};

const pmm_ops_t* pmm_bitmap_get_ops(void) {
	return &g_bitmap_ops;
}
