#include <stdint.h>
#include <stddef.h>

#include "gdt.h"

#include "serial.h"

#include "idt.h"
#include "printk.h"

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

void kmain(uint32_t mb2_magic, const void* mb2_info) {
    serial_init();
    printk("kmain: hello from C\n");

    printk("gdt: before init\n");
    gdt_init();
    printk("gdt: after init\n");

    printk("idt: before init\n");
    idt_init();
    printk("idt: after init\n");

    printk("mb2_magic=%08x\n", mb2_magic);

    if (mb2_magic != 0x36d76289u) {
        printk("[mb2] bad magic\n");
    } else {
        mb2_dump_tags(mb2_info);
    }

    /* --- IRQ + keyboard + shell ---
     *
     * 现在我们已经把 IRQ1 键盘中断翻译成 ASCII 字符流（见 keyboard.c）。
     * 所以这里直接进入 shell：
     *   szy-kernel > help
     *   szy-kernel > info
     *   szy-kernel > cls
     */
    pic_init();
    keyboard_init();

    printk("[kbd] IRQ1 enabled. Starting shell...\n");
    __asm__ volatile ("sti");

    shell_run();
}
