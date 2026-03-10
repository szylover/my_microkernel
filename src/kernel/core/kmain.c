#include <stdint.h>
#include <stddef.h>

#include "gdt.h"

#include "serial.h"

#include "idt.h"
#include "printk.h"

#include "pmm.h"

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

static void kmain_print_login(void) {
    /*
     * “登录界面”输出到串口终端（QEMU -serial stdio）。
     * 用 ANSI 序列清屏后再画一个 ASCII Banner。
     */
    printk("\x1b[2J\x1b[H");

    printk("+------------------------------------------------------------------------------+\n");
    printk("|   _____  ________  __   __      __ ________________                         |\n");
    printk("|  / ___/ /_  __/ / / /  / /__   / //_/ __/ __/ __/ /                         |\n");
    printk("| / /__    / / / /_/ /  / / _ \\ / ,< / _// _// _// /                          |\n");
    printk("| \\___/   /_/  \\____/  /_/\\___//_/|_/___/___/___/_/                           |\n");
    printk("|                                                                              |\n");
    printk("|  %-12s v%-16s  build: %s %s                                |\n", KERNEL_NAME, KERNEL_VERSION, __DATE__, __TIME__);
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

        /* Stage-2: physical memory manager (bitmap allocator). */
        pmm_init();
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
