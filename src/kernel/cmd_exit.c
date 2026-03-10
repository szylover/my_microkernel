#include "cmd.h"

#include "printk.h"
#include "shell.h"

static int cmd_exit_main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    printk("Exiting shell...\n");
    shell_request_exit();
    return 0;
}

const cmd_t cmd_exit = {
    .name = "exit",
    .help = "Exit the shell",
    .fn = cmd_exit_main,
};
