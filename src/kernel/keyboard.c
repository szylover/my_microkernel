#include "keyboard.h"

#include "io.h"
#include "irq.h"
#include "pic.h"
#include "printk.h"

/*
 * PS/2 键盘数据端口：0x60
 * 状态端口：0x64（此最小版本不需要用到）
 */

enum {
    KBD_DATA = 0x60,
};

static void keyboard_irq1_handler(const irq_regs_t* r) {
    (void)r;

    /*
     * 必须读取一次 0x60（scancode），否则键盘控制器可能保持“数据未读”状态。
     * scancode 高位 1 通常表示 key release；0 表示 key press。
     */
    uint8_t sc = inb(KBD_DATA);

    /* 临时测试：仅对按下（make code）输出提示符，避免按键抬起也刷屏。 */
    if ((sc & 0x80u) == 0) {
        printk("SzyOs >\n");
    }
}

void keyboard_init(void) {
    /* 安装 IRQ1 handler 并打开 PIC 的 IRQ1 mask。 */
    irq_install_handler(1, keyboard_irq1_handler);

    /* IRQ1 在 master PIC 上（键盘），打开它。 */
    pic_clear_mask(1);
}
