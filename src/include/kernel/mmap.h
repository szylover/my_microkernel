#pragma once

/*
 * mmap.h — Multiboot2 memory map dump helper
 *
 * This is a small utility used by the shell command `mmap`.
 * It parses the Multiboot2 information structure and prints a human-readable
 * memory map plus a summary of available RAM.
 */

#ifdef __cplusplus
extern "C" {
#endif

void mmap_print(void);

#ifdef __cplusplus
}
#endif
