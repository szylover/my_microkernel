#include "shell.h"

#include <stddef.h>
#include <stdint.h>

#include "keyboard.h"
#include "printk.h"

#define SHELL_PROMPT "szy-kernel > "
#define SHELL_LINE_MAX 128

typedef int (*cmd_fn_t)(int argc, char** argv);

typedef struct {
    const char* name;
    const char* help;
    cmd_fn_t fn;
} cmd_t;

static int streq(const char* a, const char* b) {
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int cmd_help(int argc, char** argv);
static int cmd_info(int argc, char** argv);
static int cmd_cls(int argc, char** argv);

static const cmd_t g_cmds[] = {
    {"help", "List available commands", cmd_help},
    {"info", "Print kernel build info", cmd_info},
    {"cls",  "Clear screen (serial ANSI)", cmd_cls},
};

static int cmd_help(int argc, char** argv) {
    (void)argc;
    (void)argv;

    printk("Commands:\n");
    for (size_t i = 0; i < (sizeof(g_cmds) / sizeof(g_cmds[0])); i++) {
        printk("  %s - %s\n", g_cmds[i].name, g_cmds[i].help);
    }
    return 0;
}

static int cmd_info(int argc, char** argv) {
    (void)argc;
    (void)argv;

    printk("szy-kernel (my_microkernel)\n");
    printk("build: %s %s\n", __DATE__, __TIME__);
    printk("input: ps/2 keyboard (IRQ1)\n");
    return 0;
}

static int cmd_cls(int argc, char** argv) {
    (void)argc;
    (void)argv;

    /* ANSI clear screen + cursor home (works for QEMU -serial stdio terminals). */
    printk("\x1b[2J\x1b[H");
    return 0;
}

static void shell_print_prompt(void) {
    printk(SHELL_PROMPT);
}

static size_t shell_readline(char* buf, size_t cap) {
    if (!buf || cap == 0) {
        return 0;
    }

    size_t len = 0;
    for (;;) {
        char c = keyboard_getc();

        if (c == '\n') {
            printk("\n");
            buf[len] = '\0';
            return len;
        }

        if (c == '\b' || (unsigned char)c == 0x7f) {
            if (len > 0) {
                len--;
                printk("\b \b");
            }
            continue;
        }

        /* 忽略其他控制字符 */
        if ((unsigned char)c < 0x20) {
            continue;
        }

        if (len + 1 < cap) {
            buf[len++] = c;
            printk("%c", c);
        }
    }
}

static int shell_tokenize(char* line, char** argv, int argv_cap) {
    if (!line || !argv || argv_cap <= 0) {
        return 0;
    }

    int argc = 0;
    char* p = line;

    while (*p) {
        while (*p && is_space(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        if (argc >= argv_cap) {
            break;
        }

        argv[argc++] = p;

        while (*p && !is_space(*p)) {
            p++;
        }

        if (*p) {
            *p = '\0';
            p++;
        }
    }

    return argc;
}

static void shell_dispatch(char* line) {
    if (!line || !*line) {
        return;
    }

    char* argv[16];
    int argc = shell_tokenize(line, argv, (int)(sizeof(argv) / sizeof(argv[0])));
    if (argc <= 0) {
        return;
    }

    for (size_t i = 0; i < (sizeof(g_cmds) / sizeof(g_cmds[0])); i++) {
        if (streq(argv[0], g_cmds[i].name)) {
            (void)g_cmds[i].fn(argc, argv);
            return;
        }
    }

    printk("Unknown command: %s\n", argv[0]);
}

void shell_run(void) {
    printk("\n");
    printk("Interactive shell ready. Type 'help'.\n");

    char line[SHELL_LINE_MAX];

    for (;;) {
        shell_print_prompt();
        (void)shell_readline(line, sizeof(line));
        shell_dispatch(line);
    }
}
