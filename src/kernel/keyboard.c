#include "keyboard.h"

#include "io.h"
#include "irq.h"
#include "pic.h"

/*
 * PS/2 键盘数据端口：0x60
 * 状态端口：0x64（此最小版本不需要用到）
 */

enum {
    KBD_DATA = 0x60,
};

/*
 * 键盘字符环形缓冲区（单生产者=IRQ，上下文；单消费者=主循环/shell）。
 *
 * 设计选择：
 * - 中断里只做：读 scancode、更新 shift 状态、翻译成 ASCII、写入 buffer。
 * - 主循环里做：行编辑与 printk 输出。
 *
 * 为什么：
 * - printk 可能很慢（串口轮询发送），放在中断里会拉长中断处理时间。
 * - 中断里打印还容易造成输出重入/时序混乱。
 */

#define KBD_BUF_SIZE 128

static volatile char g_kbd_buf[KBD_BUF_SIZE];
static volatile uint8_t g_kbd_head;
static volatile uint8_t g_kbd_tail;

static volatile uint8_t g_shift_down;
static volatile uint8_t g_e0_prefix;

static void kbd_buf_put(char c) {
    uint8_t head = g_kbd_head;
    uint8_t next = (uint8_t)((head + 1u) % KBD_BUF_SIZE);
    if (next == g_kbd_tail) {
        /* 缓冲满：丢弃字符（最简单策略）。 */
        return;
    }

    g_kbd_buf[head] = c;
    g_kbd_head = next;
}

static int kbd_buf_get(char* out) {
    if (!out) {
        return 0;
    }

    uint8_t tail = g_kbd_tail;
    if (tail == g_kbd_head) {
        return 0;
    }

    *out = g_kbd_buf[tail];
    g_kbd_tail = (uint8_t)((tail + 1u) % KBD_BUF_SIZE);
    return 1;
}

/*
 * Scancode Set 1 基础映射表。
 * 表索引是“make code”（按下时 scancode，0x00..0x7F）。
 * 未支持的键填 0。
 */
static const char scancode_map[0x40] = {
    /* 0x00 */ 0,  0,  '1','2','3','4','5','6','7','8','9','0','-','=',
    /* 0x0E */ '\b', '\t',
    /* 0x10 */ 'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    /* 0x1E */ 'a','s','d','f','g','h','j','k','l',';','\'', '`', 0, '\\',
    /* 0x2C */ 'z','x','c','v','b','n','m',',','.','/', 0,  0,  0,  ' ',
};

static const char scancode_map_shift[0x40] = {
    /* 0x00 */ 0,  0,  '!','@','#','$','%','^','&','*','(',')','_','+',
    /* 0x0E */ '\b', '\t',
    /* 0x10 */ 'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
    /* 0x1E */ 'A','S','D','F','G','H','J','K','L',':','"','~', 0,  '|',
    /* 0x2C */ 'Z','X','C','V','B','N','M','<','>','?', 0,  0,  0,  ' ',
};

static void keyboard_handle_scancode(uint8_t sc) {
    /* 0xE0 前缀：扩展键（方向键等）。最小版本先忽略整个扩展序列。 */
    if (sc == 0xE0u) {
        g_e0_prefix = 1;
        return;
    }

    if (g_e0_prefix) {
        /* 目前不处理扩展键，直接丢弃本次 scancode。 */
        g_e0_prefix = 0;
        return;
    }

    /* break code（抬起）：最高位=1 */
    uint8_t released = (uint8_t)((sc & 0x80u) != 0);
    uint8_t code = (uint8_t)(sc & 0x7Fu);

    /* 左右 Shift */
    if (code == 0x2Au || code == 0x36u) {
        g_shift_down = released ? 0u : 1u;
        return;
    }

    /* 只对“按下(make code)”产生字符 */
    if (released) {
        return;
    }

    if (code < (uint8_t)sizeof(scancode_map)) {
        char ch = g_shift_down ? scancode_map_shift[code] : scancode_map[code];
        if (ch) {
            kbd_buf_put(ch);
        }
    }
}

static void keyboard_irq1_handler(const irq_regs_t* r) {
    (void)r;

    /*
     * 必须读取一次 0x60（scancode），否则键盘控制器可能保持“数据未读”状态。
     * scancode 高位 1 通常表示 key release；0 表示 key press。
     */
    uint8_t sc = inb(KBD_DATA);

    keyboard_handle_scancode(sc);
}

void keyboard_init(void) {
    /* 安装 IRQ1 handler 并打开 PIC 的 IRQ1 mask。 */
    irq_install_handler(1, keyboard_irq1_handler);

    /* IRQ1 在 master PIC 上（键盘），打开它。 */
    pic_clear_mask(1);
}

int keyboard_try_getc(char* out) {
    /* 单消费者读取：这里不关中断也能工作（单字节读写），但保持接口最小。 */
    return kbd_buf_get(out);
}

char keyboard_getc(void) {
    char c;
    while (!keyboard_try_getc(&c)) {
        /*
         * 等待下一次中断把字符塞进 buffer。
         * 前提：外部已执行 sti（IF=1）。
         */
        __asm__ volatile("hlt");
    }
    return c;
}
