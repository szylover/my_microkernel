#include <stdarg.h>
#include <stdint.h>

#include "printk.h"
#include "serial.h"

static void print_hex_u32(uint32_t num) {
    char hex[8];

    if (num == 0) {
        serial_putc('0');
        return;
    }

    int i = 0;
    while (num > 0 && i < (int)sizeof(hex)) {
        uint8_t digit = (uint8_t)(num & 0xFu);
        hex[i++] = (char)((digit < 10) ? ('0' + digit) : ('a' + (digit - 10)));
        num >>= 4;
    }

    for (int j = i - 1; j >= 0; j--) {
        serial_putc(hex[j]);
    }
}

static void print_dec_u32(uint32_t num) {
    char dec[10];

    if (num == 0) {
        serial_putc('0');
        return;
    }

    int i = 0;
    while (num > 0 && i < (int)sizeof(dec)) {
        uint32_t digit = num % 10u;
        dec[i++] = (char)('0' + digit);
        num /= 10u;
    }

    for (int j = i - 1; j >= 0; j--) {
        serial_putc(dec[j]);
    }
}

static void print_int(int value) {
    if (value < 0) {
        serial_putc('-');
        /*
         * 不能直接做 -value：当 value == INT_MIN 时会溢出。
         * 转成无符号做补码绝对值：abs = 0 - (uint32_t)value
         */
        uint32_t abs_u = 0u - (uint32_t)value;
        print_dec_u32(abs_u);
        return;
    }

    print_dec_u32((uint32_t)value);
}

void vprintk(const char* fmt, va_list args) {
    while (fmt && *fmt) {
        if (*fmt != '%') {
            serial_putc(*fmt++);
            continue;
        }

        fmt++; /* skip '%' */
        if (*fmt == '\0') {
            /* fmt 以 '%' 结尾：按字面输出 '%' */
            serial_putc('%');
            break;
        }

        switch (*fmt) {
            case '%':
                serial_putc('%');
                break;
            case 'd':
                print_int(va_arg(args, int));
                break;
            case 'u':
                print_dec_u32(va_arg(args, unsigned int));
                break;
            case 'x':
                print_hex_u32(va_arg(args, unsigned int));
                break;
            case 's': {
                const char* str = va_arg(args, const char*);
                serial_write(str ? str : "(null)");
                break;
            }
            case 'c':
                serial_putc((char)va_arg(args, int));
                break;
            default:
                serial_putc('%');
                serial_putc(*fmt);
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