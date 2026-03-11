
#pragma once

/*
 * pmm.h — Physical Memory Manager (PMM)
 *
 * Stage-2 goal (see docs/agent.md): build a simple 4KiB-page allocator using a
 * bitmap, based on the Multiboot2 memory map "best available region".
 *
 * Design notes:
 * - Page size is fixed to 4096 bytes.
 * - The allocator returns *physical* page addresses. Today the kernel runs with
 *   identity mapping (no paging), so these are also valid pointers.
 * - No dynamic allocation is used; the bitmap lives inside the chosen RAM
 *   region itself.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the PMM using the Multiboot2 info pointer saved by kmain. */
void pmm_init(void);

/* Allocate one 4KiB physical page. Returns NULL on OOM / not initialized. */
void* pmm_alloc_page(void);

/* Free a page previously returned by pmm_alloc_page(). */
void pmm_free_page(void* page);

/* Query helpers for the `free` shell command. */
unsigned pmm_total_pages(void);
unsigned pmm_free_pages(void);

/* Debug/inspection helpers for shell commands.
 *
 * Returns 0 if PMM is not initialized.
 */
unsigned pmm_managed_base(void);

/* Query whether a page (by PMM page index) is marked used/reserved.
 * Return values:
 *   1  => used/reserved
 *   0  => free
 *  -1  => PMM not initialized or index out of range
 */
int pmm_page_is_used(unsigned page_index);

#ifdef __cplusplus
}
#endif

