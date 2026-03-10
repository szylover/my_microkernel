#include "cmd.h"

#include "io.h"
#include "printk.h"

static void cpu_halt_forever(void) {
    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static int cmd_shutdown_main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    printk("Shutting down...\n");

    /*
     * 尝试在常见虚拟机/模拟器里触发关机：
     * - QEMU (常见): outw(0x604, 0x2000)
     * - Bochs (常见): outw(0xB004, 0x2000)
     *
     * 若平台不支持，这些写端口会无效果，于是退化为 halt。
     */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);

    cpu_halt_forever();
    return 0;
}

const cmd_t cmd_shutdown = {
    .name = "shutdown",
    .help = "Power off (QEMU/Bochs) or halt",
    .fn = cmd_shutdown_main,
};
