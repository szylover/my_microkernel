#include "console.h"

#include "keyboard.h"
#include "serial.h"

int console_try_getc(char* out) {
    if (!out) {
        return 0;
    }

    if (keyboard_try_getc(out)) {
        return 1;
    }

    if (serial_try_getc(out)) {
        if (*out == '\r') {
            *out = '\n';
        }
        return 1;
    }

    return 0;
}

char console_getc(void) {
    for (;;) {
        char c;
        if (console_try_getc(&c)) {
            return c;
        }

        /*
         * 串口接收是轮询，没有 IRQ 唤醒；用 pause 降低忙等开销。
         *（如果未来打开 IRQ0 时钟，也可以改成 hlt + 定期 wake。）
         */
        __asm__ volatile("pause");
    }
}
