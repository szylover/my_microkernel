#pragma once

#include <stdint.h>

/*
 * GDT (Global Descriptor Table) — x86 分段机制的“描述符表”。
 *
 * 你现在处在 i386 32-bit 保护模式下（GRUB/Multiboot2 已经帮你进入）。
 * 这时候 CPU 的每个段寄存器（CS/DS/SS/ES/FS/GS）并不是直接存 base/limit，
 * 而是存一个“选择子 selector”。CPU 会用 selector 去 GDT/LDT 查表，
 * 把找到的描述符缓存到段寄存器的 hidden part（不可直接读写的缓存）里。
 *
 * 为什么我们还要“自己再初始化一次 GDT”？
 * - 引导器给你的 GDT 是什么样你不一定清楚；为了可控、可复现，内核通常会重新设置。
 * - 后面做 IDT、TSS、用户态（DPL=3）等，都要依赖一个你自己掌控的 GDT。
 * - 即使将来上 x86_64 长模式，GDT 仍然需要（尤其 TSS），所以这一步是必学基础。
 *
 * 这里我们实现最小的“平坦模型 Flat Segmentation”：
 * - base=0
 * - limit=4GiB
 * - code/data 段都覆盖整个线性地址空间
 *
 * 这样分段不会“参与地址转换”（效果上类似关掉分段的影响），
 * 让你能把注意力集中到分页/中断等主题上。
 */

/*
 * 选择子（selector）格式（16-bit）：
 * - bits 3..15: index（第几个描述符）
 * - bit 2: TI（0=GDT, 1=LDT）
 * - bits 0..1: RPL（请求特权级，0..3）
 *
 * 我们的 GDT：
 * - index 0: null
 * - index 1: kernel code
 * - index 2: kernel data
 */

enum {
    GDT_KERNEL_CODE_SEL = 0x08, /* 1 << 3 */
    GDT_KERNEL_DATA_SEL = 0x10, /* 2 << 3 */
};

/* 初始化并加载 GDT，同时刷新 CS/DS/SS 等段寄存器。 */
void gdt_init(void);
