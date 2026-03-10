#include "shell.h"

#include <stddef.h>

#include "cmd.h"
#include "console.h"
#include "printk.h"

#define SHELL_PROMPT "szy-kernel > "
#define SHELL_LINE_MAX 128

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
extern const cmd_t cmd_shutdown;
extern const cmd_t cmd_cmds;
extern const cmd_t cmd_mmap;

static const cmd_t* g_cmds[] = {
    &cmd_cls,
    &cmd_shutdown,
    &cmd_cmds,
    &cmd_mmap,
};

unsigned shell_command_count(void) {
    return (unsigned)(sizeof(g_cmds) / sizeof(g_cmds[0]));
}

void shell_print_commands(void) {
    /*
     * Pretty output: fixed-width multi-column list of command names.
     * We assume a typical 80-column monospace terminal (QEMU -serial stdio).
     */
    const unsigned term_width = 80;

    unsigned count = shell_command_count();
    printk("Commands (%u):\n", count);

    size_t max_len = 0;
    for (size_t i = 0; i < (sizeof(g_cmds) / sizeof(g_cmds[0])); i++) {
        const char* s = g_cmds[i]->name;
        size_t n = 0;
        while (s && s[n]) {
            n++;
        }
        if (n > max_len) {
            max_len = n;
        }
    }

    /* +2 for spacing between columns. Minimum width 4 to keep things readable. */
    unsigned col_w = (unsigned)(max_len + 2);
    if (col_w < 4) {
        col_w = 4;
    }

    unsigned cols = term_width / col_w;
    if (cols == 0) {
        cols = 1;
    }

    for (size_t i = 0; i < (sizeof(g_cmds) / sizeof(g_cmds[0])); i++) {
        const char* name = g_cmds[i]->name;

        printk("%s", name ? name : "");

        /* Pad to column width (except possibly end of line). */
        size_t n = 0;
        while (name && name[n]) {
            n++;
        }
        unsigned pad = (col_w > (unsigned)n) ? (col_w - (unsigned)n) : 1;
        for (unsigned p = 0; p < pad; p++) {
            printk(" ");
        }

        if (((unsigned)(i + 1)) % cols == 0) {
            printk("\n");
        }
    }

    if (((unsigned)count) % cols != 0) {
        printk("\n");
    }
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

    /*
     * NOTE (future design):
     * 之后我们会引入“上层 monitor/login”这样的层级。
     * - 在 shell 里执行 `exit`：退出到上层 monitor
     * - 如果 monitor 已经是最上层：再退出则回到 login
     *
     * 现在仓库还没做层级设计，所以 shell 暂不提供 `exit`。
     */

    char line[SHELL_LINE_MAX];

    for (;;) {
        shell_print_prompt();
        (void)shell_readline(line, sizeof(line));
        shell_dispatch(line);
    }
}
