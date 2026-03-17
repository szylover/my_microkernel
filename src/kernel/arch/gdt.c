#include "gdt.h"

/*
 * 这一文件是“学习级”实现：会把每个字段的含义写清楚。
 *
 * GDT 描述符（segment descriptor）是一个 8 字节结构，布局固定。
 * Intel 手册里叫：Segment Descriptor / Code-Data Segment Descriptor。
 *
 * 对于 code/data 段（不考虑 TSS/LDT 等系统段），字段含义概览：
 * - base: 段基址（线性地址）
 * - limit: 段界限（段长度 - 1），配合 G 位（粒度）决定单位
 * - access byte（访问字节）：P/DPL/S/Type
 * - flags+limit_high：G/D-B/L/AVL + limit 高 4 位
 *
 * 我们做平坦模型：base=0，limit=0xFFFFF（配合 G=1 表示 4GiB-1）。
 */

typedef struct __attribute__((packed)) {
    uint16_t limit_low;   // limit[0:15]
    uint16_t base_low;    // base[0:15]
    uint8_t  base_mid;    // base[16:23]
    uint8_t  access;      // 访问字节：P DPL S Type
    uint8_t  gran;        // flags(高4位) + limit[16:19](低4位)
    uint8_t  base_high;   // base[24:31]
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit; // GDT 字节大小 - 1
    uint32_t base;  // GDT 线性地址
} gdtr_t;

/*
 * access 字节（代码段/数据段通用格式）：
 *
 * bit 7: P (Present)                 1=描述符有效
 * bit 6-5: DPL (Descriptor Privilege Level)
 * bit 4: S (Descriptor type)         1=code/data 段，0=系统段（TSS/LDT 等）
 * bit 3-0: Type
 *   对 code 段：
 *     bit3=1 (executable)
 *     bit1=1 (readable)
 *   对 data 段：
 *     bit3=0 (data)
 *     bit1=1 (writable)
 *
 * 常用组合：
 * - 0x9A = 10011010b = P=1, DPL=0, S=1, Type=1010(code, read)
 * - 0x92 = 10010010b = P=1, DPL=0, S=1, Type=0010(data, write)
 */

/*
 * gran 字节：高 4 位是 flags，低 4 位是 limit[16:19]
 *
 * flags：
 * bit 7: G  (Granularity) 1=limit 单位 4KiB，0=byte
 * bit 6: D/B(Default op size) 1=32-bit 段，0=16-bit 段
 * bit 5: L  (64-bit code segment) 1=长模式代码段（i386 下应为 0）
 * bit 4: AVL(Available) 给软件自用
 */

/*
 * GDT 条目数量：
 *   [0] null  [1] kernel code  [2] kernel data
 *   [3] user code  [4] user data  [5] TSS
 */
static gdt_entry_t gdt[6];
static gdtr_t gdtr;

/* 汇编实现：lgdt + far jump 刷新 CS + reload DS/SS/... */
extern void gdt_flush(const gdtr_t* gdtr_ptr);

static void gdt_set_entry(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    /*
     * limit 在 G=1（4KiB 粒度）时，最大可表示：0xFFFFF * 4KiB + 0xFFF = 4GiB - 1
     * 所以平坦模型通常：limit = 0xFFFFF 且 G=1。
     */
    gdt[idx].limit_low = (uint16_t)(limit & 0xFFFF);

    gdt[idx].base_low = (uint16_t)(base & 0xFFFF);
    gdt[idx].base_mid = (uint8_t)((base >> 16) & 0xFF);
    gdt[idx].base_high = (uint8_t)((base >> 24) & 0xFF);

    gdt[idx].access = access;

    /* gran: high 4 bits = flags, low 4 bits = limit high */
    gdt[idx].gran = (uint8_t)((flags & 0xF0) | ((limit >> 16) & 0x0F));
}

void gdt_init(void) {
    /*
     * descriptor 0 必须是空描述符（null descriptor）。
     * 当 selector=0 时表示“空段”，CPU 会在某些场景用它做检查。
     */
    gdt_set_entry(0, 0, 0, 0, 0);

    /*
     * descriptor 1：内核代码段
     * base=0，limit=0xFFFFF（配合 G=1 表示 4GiB-1），DPL=0
     */
    gdt_set_entry(
        1,
        0x00000000,
        0x000FFFFF,
        0x9A, /* access: present, ring0, code, readable */
        0xC0  /* flags: G=1 (0x80), D/B=1 (0x40), L=0, AVL=0 */
    );

    /*
     * descriptor 2：内核数据段
     * 仍然是平坦段，但 Type 变成 data/writable。
     */
    gdt_set_entry(
        2,
        0x00000000,
        0x000FFFFF,
        0x92, /* access: present, ring0, data, writable */
        0xC0
    );

    /*
     * descriptor 3：用户代码段  (selector = 0x18, 加上 RPL=3 → 0x1B)
     *
     * [WHY] Ring 3 代码需要运行在 DPL=3 的段中。
     *   与内核代码段的唯一区别：
     *   access 中 DPL 从 0 改为 3 (bits 6-5 = 11b)
     *
     * [BITFIELDS] access = 0xFA = 11111010b
     *   P=1      描述符有效
     *   DPL=3    用户态特权级
     *   S=1      代码/数据段（非系统段）
     *   Type=1010  可执行 + 可读
     */
    gdt_set_entry(
        3,
        0x00000000,
        0x000FFFFF,
        0xFA, /* access: present, ring3, code, readable */
        0xC0  /* flags: G=1, D/B=1 (32-bit) */
    );

    /*
     * descriptor 4：用户数据段  (selector = 0x20, 加上 RPL=3 → 0x23)
     *
     * [WHY] Ring 3 代码的 DS/SS/ES 需要用 DPL=3 的数据段。
     *   与内核数据段的唯一区别：DPL = 3
     *
     * [BITFIELDS] access = 0xF2 = 11110010b
     *   P=1, DPL=3, S=1, Type=0010 (data, writable)
     */
    gdt_set_entry(
        4,
        0x00000000,
        0x000FFFFF,
        0xF2, /* access: present, ring3, data, writable */
        0xC0
    );

    /*
     * descriptor 5：TSS 占位
     * 此处先设为空，tss_init() 会通过 gdt_set_tss_entry() 填写
     * 实际的 base（TSS 结构体地址）和 limit（sizeof(tss)-1）。
     */
    gdt_set_entry(5, 0, 0, 0, 0);

    /*
     * GDTR（lgdt 指令的操作数）:
     * - limit = sizeof(GDT) - 1
     * - base  = GDT 的线性地址
     */
    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base = (uint32_t)(uintptr_t)&gdt[0];

    /*
     * 关键步骤：
     * 1) lgdt 只是“告诉 CPU 新表在哪里”，并不会自动更新 CS/DS/SS 的 hidden cache。
     * 2) 必须执行一次 far jump（或 far call/iret）来刷新 CS。
     * 3) 必须重新加载 DS/ES/SS/FS/GS，才能让它们的 hidden cache 指向新描述符。
     */
    gdt_flush(&gdtr);
}

/*
 * gdt_set_tss_entry — 在 GDT[5] 写入 TSS 描述符
 *
 * [WHY] TSS 描述符是"系统段"描述符（S=0），和代码/数据段（S=1）格式不同。
 *   type = 0x89 表示 "32-bit TSS (Available)"
 *
 * [BITFIELDS] TSS 描述符 access 字节：
 *   bit 7   : P=1       (描述符有效)
 *   bit 6-5 : DPL=0     (只有内核能 ltr)
 *   bit 4   : S=0       (系统段，非代码/数据)
 *   bit 3-0 : type=1001 (32-bit TSS Available)
 *   合并: 0x89 = 10001001b
 *
 * [BITFIELDS] flags 字节（gran 高 4 位）：
 *   G=0   (粒度为字节，TSS limit 通常 < 4KiB)
 *   D/B=0 (TSS 描述符中无意义)
 *   L=0, AVL=0
 *   合并: 0x00
 */
void gdt_set_tss_entry(uint32_t base, uint32_t limit) {
    gdt_set_entry(
        5,
        base,
        limit,
        0x89, /* access: P=1, DPL=0, S=0, type=1001 (32-bit TSS Available) */
        0x00  /* flags: G=0, D/B=0 */
    );
}
