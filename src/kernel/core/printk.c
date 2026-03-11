#include <stdarg.h>
#include <stdint.h>

#include "printk.h"
#include "serial.h"

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int cstr_len(const char* s) {
    if (!s) {
        return 0;
    }
    int n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void serial_putn(char ch, int n) {
    for (int i = 0; i < n; i++) {
        serial_putc(ch);
    }
}

static void serial_write_n(const char* s, int n) {
    for (int i = 0; i < n; i++) {
        serial_putc(s[i]);
    }
}

static void serial_write_padded(const char* s, int len, int width, int left_align, char pad) {
    if (width < 0) {
        width = 0;
    }

    int padding = (width > len) ? (width - len) : 0;

    if (!left_align) {
        serial_putn(pad, padding);
    }

    serial_write_n(s, len);

    if (left_align) {
        serial_putn(' ', padding);
    }
}

static int fmt_u32_dec(char* out, uint32_t value) {
    /* returns length, no terminator */
    if (value == 0) {
        out[0] = '0';
        return 1;
    }

    char tmp[10];
    int i = 0;
    while (value > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    int len = 0;
    for (int j = i - 1; j >= 0; j--) {
        out[len++] = tmp[j];
    }
    return len;
}

static int fmt_int_dec(char* out, int value) {
    if (value < 0) {
        out[0] = '-';
        /* abs for INT_MIN without overflow: abs_u = 0 - (uint32_t)value */
        uint32_t abs_u = 0u - (uint32_t)value;
        return 1 + fmt_u32_dec(out + 1, abs_u);
    }
    return fmt_u32_dec(out, (uint32_t)value);
}

static int fmt_u32_hex(char* out, uint32_t value) {
    static const char* hex = "0123456789abcdef";

    if (value == 0) {
        out[0] = '0';
        return 1;
    }

    char tmp[8];
    int i = 0;
    while (value > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = hex[value & 0xFu];
        value >>= 4;
    }

    int len = 0;
    for (int j = i - 1; j >= 0; j--) {
        out[len++] = tmp[j];
    }
    return len;
}

static void print_hex_u32_width(uint32_t num, int width) {
    static const char* hex = "0123456789abcdef";

    if (width <= 0) {
        width = 1;
    }
    if (width > 8) {
        width = 8;
    }

    for (int i = width - 1; i >= 0; i--) {
        uint8_t digit = (uint8_t)((num >> (i * 4)) & 0xFu);
        serial_putc(hex[digit]);
    }
}

static void vprintk(const char* fmt, va_list args) {
    while (fmt && *fmt) {
        if (*fmt != '%') {
            serial_putc(*fmt++);
            continue;
        }

        fmt++; /* skip '%' */
        if (*fmt == '\0') {
            /* fmt ends with '%': print it literally */
            serial_putc('%');
            break;
        }

        /* flags */
        int left_align = 0;
        char pad = ' ';

        for (;;) {
            if (*fmt == '-') {
                left_align = 1;
                fmt++;
                continue;
            }
            if (*fmt == '0') {
                pad = '0';
                fmt++;
                continue;
            }
            break;
        }

        /* width */
        int width = 0;
        while (is_digit(*fmt)) {
            width = (width * 10) + (*fmt - '0');
            fmt++;
        }

        if (left_align) {
            pad = ' ';
        }

        char spec = *fmt;
        switch (spec) {
            case '%':
                serial_putc('%');
                break;
            case 'd': {
                char buf[16];
                int len = fmt_int_dec(buf, va_arg(args, int));

                /* if negative and zero padded: keep '-' first, then zeros */
                if (pad == '0' && !left_align && width > len && len > 0 && buf[0] == '-') {
                    serial_putc('-');
                    serial_putn('0', width - len);
                    serial_write_n(buf + 1, len - 1);
                } else {
                    serial_write_padded(buf, len, width, left_align, pad);
                }
                break;
            }
            case 'u': {
                char buf[16];
                int len = fmt_u32_dec(buf, (uint32_t)va_arg(args, unsigned int));
                serial_write_padded(buf, len, width, left_align, pad);
                break;
            }
            case 'x': {
                char buf[16];
                int len = fmt_u32_hex(buf, (uint32_t)va_arg(args, unsigned int));
                serial_write_padded(buf, len, width, left_align, pad);
                break;
            }
            case 'p': {
                /* 32-bit kernel: print pointers as 0xXXXXXXXX */
                uintptr_t p = (uintptr_t)va_arg(args, void*);
                serial_write("0x");
                print_hex_u32_width((uint32_t)p, 8);
                break;
            }
            case 's': {
                const char* str = va_arg(args, const char*);
                const char* out = str ? str : "(null)";
                serial_write_padded(out, cstr_len(out), width, left_align, pad);
                break;
            }
            case 'c': {
                char ch = (char)va_arg(args, int);
                serial_write_padded(&ch, 1, width, left_align, pad);
                break;
            }
            default:
                serial_putc('%');
                serial_putc(spec);
                break;
        }

        fmt++;
    }
}

void printk(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);
}
