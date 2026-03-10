#include <stdint.h>
#include <stddef.h>

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

enum {
    COM1 = 0x3F8,
};

static int serial_transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

static void serial_putc(char c) {
    while (!serial_transmit_empty()) {
        // spin
    }
    outb(COM1, (uint8_t)c);
}

static void serial_write(const char* s) {
    for (; *s; s++) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s);
    }
}

static void serial_write_hex32(uint32_t v) {
    static const char* hex = "0123456789ABCDEF";
    serial_write("0x");
    for (int i = 7; i >= 0; i--) {
        serial_putc(hex[(v >> (i * 4)) & 0xF]);
    }
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00); // Disable all interrupts
    outb(COM1 + 3, 0x80); // Enable DLAB
    outb(COM1 + 0, 0x03); // Divisor low byte (38400 baud)
    outb(COM1 + 1, 0x00); // Divisor high byte
    outb(COM1 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7); // Enable FIFO, clear, 14-byte threshold
    outb(COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

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
    serial_write("kmain: hello from C\n");

    serial_write("mb2_magic=");
    serial_write_hex32(mb2_magic);
    serial_write("\n");

    if (mb2_magic != 0x36d76289u) {
        serial_write("[mb2] bad magic\n");
    } else {
        mb2_dump_tags(mb2_info);
    }

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
