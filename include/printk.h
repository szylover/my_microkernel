
#ifndef PRINTK_H
#define PRINTK_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PRINTK_ATTR_PRINTF(fmt_idx, first_arg_idx) __attribute__((format(printf, fmt_idx, first_arg_idx)))
#else
#define PRINTK_ATTR_PRINTF(fmt_idx, first_arg_idx)
#endif

/*
 * printk: 内核早期日志输出（目前通过串口）。
 *
 * 支持的最小格式：
 * - %%  输出字符 '%'
 * - %d  有符号十进制（int）
 * - %u  无符号十进制（unsigned int）
 * - %x  无符号十六进制（unsigned int，小写，不带 0x）
 * - %0<width>x  十六进制零填充宽度（仅支持 32-bit，常用如 %08x）
 * - %p  指针（打印为 0xXXXXXXXX）
 * - %s  C 字符串（const char*，允许传 NULL）
 * - %c  单字符（int，按 char 输出）
 */
void printk(const char* fmt, ...) PRINTK_ATTR_PRINTF(1, 2);

#undef PRINTK_ATTR_PRINTF

#ifdef __cplusplus
}
#endif

#endif /* PRINTK_H */
