#include <stdint.h>
#include <stddef.h>

#include "gdt.h"

#include "serial.h"

#include "idt.h"
#include "printk.h"

// Multiboot2 information structure
struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

static void mb2_dump_tags(const void* mb2_info) {
    const uint8_t* base = (const uint8_t*)mb2_info;
    uint32_t total_size = *(const uint32_t*)(base + 0);

    serial_write("[mb2] info @ ");
    serial_write_hex32((uint32_t)(uintptr_t)mb2_info);
    serial_write(", total_size=");
    serial_write_hex32(total_size);
    serial_write("\n");

    const uint8_t* p = base + 8;
    const uint8_t* end = base + total_size;

    while (p + sizeof(struct mb2_tag) <= end) {
        const struct mb2_tag* tag = (const struct mb2_tag*)p;
        if (tag->type == 0 && tag->size == 8) {
            serial_write("[mb2] end tag\n");
            break;
        }

        serial_write("[mb2] tag type=");
        serial_write_hex32(tag->type);
        serial_write(", size=");
        serial_write_hex32(tag->size);
        serial_write("\n");

        if (tag->size < 8) {
            serial_write("[mb2] invalid tag size\n");
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

    printk("mb2_magic=");
    printk("%x\n", mb2_magic);

    if (mb2_magic != 0x36d76289u) {
        printk("[mb2] bad magic\n");
    } else {
        mb2_dump_tags(mb2_info);
    }

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
