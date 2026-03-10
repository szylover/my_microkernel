#include "cmd.h"

#include "mmap.h"

static int cmd_mmap_main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    mmap_print();
    return 0;
}

const cmd_t cmd_mmap = {
    .name = "mmap",
    .help = "Dump Multiboot2 memory map and show available RAM",
    .fn = cmd_mmap_main,
};
