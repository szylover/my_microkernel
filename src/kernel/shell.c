#include "shell.h"

#include <stddef.h>

#include "cmd.h"
#include "console.h"
#include "printk.h"

#define SHELL_PROMPT "szy-kernel > "
#define SHELL_LINE_MAX 128

static volatile int g_shell_exit_requested = 0;

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

extern const cmd_t cmd_cls;
extern const cmd_t cmd_exit;

static const cmd_t* g_cmds[] = {
    &cmd_cls,
    &cmd_exit,
};

void shell_request_exit(void) {
    g_shell_exit_requested = 1;
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
        char c = console_getc();

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
        const cmd_t* cmd = g_cmds[i];
        if (streq(argv[0], cmd->name)) {
            (void)cmd->fn(argc, argv);
            return;
        }
    }

    printk("Unknown command: %s\n", argv[0]);
}

void shell_run(void) {
    printk("\n");
    printk("Interactive shell ready.\n");

    g_shell_exit_requested = 0;

    char line[SHELL_LINE_MAX];

    for (; !g_shell_exit_requested;) {
        shell_print_prompt();
        (void)shell_readline(line, sizeof(line));
        shell_dispatch(line);
    }

    printk("Shell exited.\n");
}
