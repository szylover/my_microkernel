#pragma once

#include <stddef.h>

/*
 * console.h — 统一的“控制台输入”抽象（给 shell 用）
 *
 * 目的：把输入源选择/轮询策略从 shell 中抽出去。
 * 当前输入源：
 * - PS/2 键盘：IRQ1 -> ring buffer（keyboard_try_getc）
 * - 串口：COM1 轮询接收（serial_try_getc）
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * console_try_getc:
 * - 若无输入可读，返回 0
 * - 若读到一个字符，写入 *out 并返回 1
 *
 * 约定：会把常见的 '\r' 归一化为 '\n'，方便上层行编辑。
 */
int console_try_getc(char* out);

/* console_getc: 阻塞直到读到一个字符并返回。 */
char console_getc(void);

#ifdef __cplusplus
}
#endif
