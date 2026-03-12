#include <stdint.h>
#include <stddef.h>

#include "gdt.h"

#include "serial.h"

#include "idt.h"
#include "printk.h"

#include "pmm.h"
#include "pmm_bitmap.h"
#include "pmm_buddy.h"
#include "vmm.h"

#include "pic.h"
#include "keyboard.h"
#include "shell.h"

// Multiboot2 information structure
struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

static void mb2_dump_tags(const void* mb2_info) {
    const uint8_t* base = (const uint8_t*)mb2_info;
    uint32_t total_size = *(const uint32_t*)(base + 0);

    printk("[mb2] info @ %p, total_size=%u\n", mb2_info, total_size);

    const uint8_t* p = base + 8;
    const uint8_t* end = base + total_size;

    while (p + sizeof(struct mb2_tag) <= end) {
        const struct mb2_tag* tag = (const struct mb2_tag*)p;
        if (tag->type == 0 && tag->size == 8) {
            printk("[mb2] end tag\n");
            break;
        }

        printk("[mb2] tag type=%u, size=%u\n", tag->type, tag->size);

        if (tag->size < 8) {
            printk("[mb2] invalid tag size\n");
            break;
        }

        // tags are 8-byte aligned
        uint32_t next = (tag->size + 7u) & ~7u;
        p += next;
    }
}

#define KERNEL_NAME "SZY-KERNEL"
#define KERNEL_VERSION "0.1.0-dev"

/*
 * Multiboot2 info pointer (provided by GRUB).
 *
 * We keep it as a global so that later commands (e.g. `mmap`) can parse
 * Multiboot2 tags from within the shell.
 */
const void* g_mb2_info = NULL;

static int kstrlen(const char* s) {
    if (!s) {
        return 0;
    }
    int n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void banner_pad_to_inner_width(int already_printed) {
    /* Banner is: |<78 chars>|
     * We print content after the leading '|', then pad with spaces to reach 78.
     */
    const int inner_width = 78;
    int remain = inner_width - already_printed;
    if (remain < 0) {
        remain = 0;
    }
    for (int i = 0; i < remain; i++) {
        printk("%c", ' ');
    }
}

static void kmain_print_login(void) {
    /*
     * “登录界面”输出到串口终端（QEMU -serial stdio）。
     * 用 ANSI 序列清屏后再画一个 ASCII Banner。
     */
    printk("\x1b[2J\x1b[H");

    printk("+------------------------------------------------------------------------------+\n");
    printk("|   _____  ________  __   __      __ ________________                          |\n");
    printk("|  / ___/ /_  __/ / / /  / /__   / //_/ __/ __/ __/ /                          |\n");
    printk("| / /__    / / / /_/ /  / / _ \\ / ,< / _// _// _// /                           |\n");
    printk("| \\___/   /_/  \\____/  /_/\\___//_/|_/___/___/___/_/                            |\n");
    printk("|                                                                              |\n");

    /* Build info line: pad dynamically so the right border stays aligned even
     * if printk implements field widths (or if strings change).
     */
    printk("|");
    printk("  %-12s v%-16s  build: %s %s", KERNEL_NAME, KERNEL_VERSION, __DATE__, __TIME__);

    int name_w = kstrlen(KERNEL_NAME);
    if (name_w < 12) name_w = 12;
    int ver_w = kstrlen(KERNEL_VERSION);
    if (ver_w < 16) ver_w = 16;

    int already = 2 /* two leading spaces */
                + name_w
                + 2 /* " v" */
                + ver_w
                + 9 /* "  build: " */
                + kstrlen(__DATE__)
                + 1 /* space between date/time */
                + kstrlen(__TIME__);

    banner_pad_to_inner_width(already);
    printk("|\n");
    printk("|  input: keyboard (IRQ1) + serial (COM1 polling)                              |\n");
    printk("|  tip  : type 'cls' then Enter to clear                                       |\n");
    printk("+------------------------------------------------------------------------------+\n");
    printk("\n");
}

void kmain(uint32_t mb2_magic, const void* mb2_info) {
    serial_init();

    gdt_init();
    printk("[init] gdt ok\n");

    idt_init();
    printk("[init] idt ok\n");

    printk("[mb2] magic=%08x\n", mb2_magic);

    if (mb2_magic != 0x36d76289u) {
        printk("[mb2] bad magic\n");
    } else {
        g_mb2_info = mb2_info;
        mb2_dump_tags(mb2_info);

        /*
         * Stage-8: 选择 PMM 后端。
         * 两个后端都编译进内核，切换只需改这一行：
         *   pmm_register_backend(pmm_bitmap_get_ops());  // bitmap
         *   pmm_register_backend(pmm_buddy_get_ops());   // buddy
         * 不注册则 dispatch 层 fallback 到 bitmap 默认后端。
         */
        pmm_register_backend(pmm_buddy_get_ops());

        /* Stage-2: physical memory manager. */
        pmm_init();

        /* Stage-3: virtual memory manager (identity mapping + paging). */
        vmm_init();

        /*
         * PMM init 已经结束，将 g_mb2_info 从物理地址转换为虚拟地址。
         * 之后 shell 命令（mmap 等）通过 high-half 地址访问 Multiboot2 info。
         */
        g_mb2_info = (const void*)PHYS_TO_VIRT((uint32_t)(uintptr_t)g_mb2_info);

        /* 拆除 identity mapping，低地址空间留给将来的用户态进程。 */
        vmm_unmap_identity();
    }

    /* --- IRQ + keyboard + shell --- */
    pic_init();
    keyboard_init();

    /* 进入交互前展示“登录界面”。 */
    kmain_print_login();

    printk("[kbd] IRQ1 enabled. Starting shell...\n");
    __asm__ volatile ("sti");

    shell_run();
}
