#pragma once

#include <stdint.h>

/*
 * keyboard.h — PS/2 键盘（最小 IRQ1 处理）
 *
 * 目前仅用于验证 IRQ1 通路是否打通：
 * - 按下任意键（make code）就在串口输出一次 "SzyOs > "。
 *
 * 后续会替换成真正的字符输入：scancode -> key state -> ASCII -> shell。
 */

#ifdef __cplusplus
extern "C" {
#endif

void keyboard_init(void);

#ifdef __cplusplus
}
#endif
