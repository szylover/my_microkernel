#pragma once

/*
 * shell.h — 内核交互式 shell（cmd 风格）
 *
 * 当前版本：
 * - 输入：console_getc()（键盘 IRQ1 字符缓冲 + 串口轮询输入）
 * - 行编辑：回显、退格、回车
 * - 命令：当前仅 `cls`（命令实现以模块形式注册到 shell）
 */

#ifdef __cplusplus
extern "C" {
#endif

void shell_run(void);

#ifdef __cplusplus
}
#endif
