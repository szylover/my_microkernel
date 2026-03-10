#pragma once

#include <stdint.h>

/*
 * keyboard.h — PS/2 键盘（IRQ1）最小字符输入
 *
 * 目标：把键盘 IRQ1 中断中的 scancode 转成“可消费的字符流”（ASCII）。
 * 这一步是实现 shell 的关键：shell 不应该在中断里直接 printk（太慢且容易重入），
 * 而应该把字符放进缓冲区，由主循环读取并做行编辑。
 *
 * 当前实现范围（刻意保持最小）：
 * - Scancode Set 1（常见 PC/AT 键盘）基础映射：字母/数字/空格/回车/退格/部分符号
 * - 处理 Shift（左右 shift）
 * - 其他键（方向键、F1..F12、Ctrl/Alt、组合键）暂时忽略
 */

#ifdef __cplusplus
extern "C" {
#endif

void keyboard_init(void);

/*
 * keyboard_try_getc:
 * - 若当前没有字符可读，返回 0
 * - 若读到一个字符，写入 *out 并返回 1
 */
int keyboard_try_getc(char* out);

/* keyboard_getc: 阻塞直到读到一个字符并返回（内部会 hlt 等待中断）。 */
char keyboard_getc(void);

#ifdef __cplusplus
}
#endif
