#include "cmd.h"

#include "printk.h"
#include "shell.h"

static int cmd_cmds_main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    shell_print_commands();
    return 0;
}

const cmd_t cmd_cmds = {
    .name = "cmds",
    .help = "List all available commands",
    .fn = cmd_cmds_main,
};
