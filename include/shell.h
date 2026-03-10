#pragma once

/*
 * shell.h — 内核交互式 shell（cmd 风格）
 *
 * 当前版本：
 * - 输入：keyboard_getc()（IRQ1 驱动提供的 ASCII 字符流）
 * - 行编辑：回显、退格、回车
 * - 命令：help / info / cls（可扩展命令表）
 */

#ifdef __cplusplus
extern "C" {
#endif

void shell_run(void);

#ifdef __cplusplus
}
#endif
