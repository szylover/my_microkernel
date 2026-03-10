#include "cmd.h"

#include "printk.h"

static int cmd_cls_main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    /*
     * 当前 cls 的实现是“对串口终端发送 ANSI 清屏序列”。
     * 注意：这清的是显示串口输出的宿主终端，不是 VGA 文本屏。
     */
    printk("\x1b[2J\x1b[H");
    return 0;
}

/*
 * 以“模块”的形式导出 cls 命令。
 * shell 通过注册表引用这个对象，从而把命令实现与 shell 主体解耦。
 */
const cmd_t cmd_cls = {
    .name = "cls",
    .help = "Clear screen (serial ANSI)",
    .fn = cmd_cls_main,
};
