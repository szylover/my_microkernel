/* libc freestanding */
#include <stdint.h>
#include <stddef.h>

/* arch: CPU 初始化 */
#include "gdt.h"
#include "idt.h"
#include "pic.h"

/* core: 内存管理 */
#include "pmm.h"
#include "pmm_backends.h"
#include "vmm.h"
#include "kmalloc.h"
#include "vma.h"
#include "kconfig.h"

/* core: 内核基础设施 */
#include "printk.h"
#include "shell.h"

/* drivers */
#include "serial.h"
#include "keyboard.h"

// Multiboot2 information structure
struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

static void mb2_dump_tags(const void* mb2_info) {
    const uint8_t* base = (const uint8_t*)mb2_info;
    uint32_t total_size = *(const uint32_t*)(base + 0);

    printk("[mb2] info @ %p, total_size=%u\n", mb2_info, total_size);

    const uint8_t* p = base + 8;
    const uint8_t* end = base + total_size;

    while (p + sizeof(struct mb2_tag) <= end) {
        const struct mb2_tag* tag = (const struct mb2_tag*)p;
        if (tag->type == 0 && tag->size == 8) {
            printk("[mb2] end tag\n");
            break;
        }

        printk("[mb2] tag type=%u, size=%u\n", tag->type, tag->size);

        if (tag->size < 8) {
            printk("[mb2] invalid tag size\n");
            break;
        }

        // tags are 8-byte aligned
        uint32_t next = (tag->size + 7u) & ~7u;
        p += next;
    }
}

#define KERNEL_NAME "SZY-KERNEL"
#define KERNEL_VERSION "0.1.0-dev"

/*
 * Multiboot2 info pointer (provided by GRUB).
 *
 * We keep it as a global so that later commands (e.g. `mmap`) can parse
 * Multiboot2 tags from within the shell.
 */
const void* g_mb2_info = NULL;

static int kstrlen(const char* s) {
    if (!s) {
        return 0;
    }
    int n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void banner_pad_to_inner_width(int already_printed) {
    /* Banner is: |<78 chars>|
     * We print content after the leading '|', then pad with spaces to reach 78.
     */
    const int inner_width = 78;
    int remain = inner_width - already_printed;
    if (remain < 0) {
        remain = 0;
    }
    for (int i = 0; i < remain; i++) {
        printk("%c", ' ');
    }
}

static void kmain_print_login(void) {
    /*
     * “登录界面”输出到串口终端（QEMU -serial stdio）。
     * 用 ANSI 序列清屏后再画一个 ASCII Banner。
     */
    printk("\x1b[2J\x1b[H");

    printk("+------------------------------------------------------------------------------+\n");
    printk("|   _____  ________  __   __      __ ________________                          |\n");
    printk("|  / ___/ /_  __/ / / /  / /__   / //_/ __/ __/ __/ /                          |\n");
    printk("| / /__    / / / /_/ /  / / _ \\ / ,< / _// _// _// /                           |\n");
    printk("| \\___/   /_/  \\____/  /_/\\___//_/|_/___/___/___/_/                            |\n");
    printk("|                                                                              |\n");

    /* Build info line: pad dynamically so the right border stays aligned even
     * if printk implements field widths (or if strings change).
     */
    printk("|");
    printk("  %-12s v%-16s  build: %s %s", KERNEL_NAME, KERNEL_VERSION, __DATE__, __TIME__);

    int name_w = kstrlen(KERNEL_NAME);
    if (name_w < 12) name_w = 12;
    int ver_w = kstrlen(KERNEL_VERSION);
    if (ver_w < 16) ver_w = 16;

    int already = 2 /* two leading spaces */
                + name_w
                + 2 /* " v" */
                + ver_w
                + 9 /* "  build: " */
                + kstrlen(__DATE__)
                + 1 /* space between date/time */
                + kstrlen(__TIME__);

    banner_pad_to_inner_width(already);
    printk("|\n");
    printk("|  input: keyboard (IRQ1) + serial (COM1 polling)                              |\n");
    printk("|  tip  : type 'cls' then Enter to clear                                       |\n");
    printk("+------------------------------------------------------------------------------+\n");
    printk("\n");
}

void kmain(uint32_t mb2_magic, const void* mb2_info) {
    serial_init();

    gdt_init();
    printk("[init] gdt ok\n");

    idt_init();
    printk("[init] idt ok\n");

    printk("[mb2] magic=%08x\n", mb2_magic);

    if (mb2_magic != 0x36d76289u) {
        printk("[mb2] bad magic\n");
    } else {
        g_mb2_info = mb2_info;
        mb2_dump_tags(mb2_info);

        /*
         * PMM 后端选择（见 kconfig.h KCONFIG_PMM_BACKEND）
         */
#if KCONFIG_PMM_BACKEND == 0
        pmm_register_backend(pmm_bitmap_get_ops());
#elif KCONFIG_PMM_BACKEND == 1
        pmm_register_backend(pmm_buddy_get_ops());
#endif

        /* Stage-2: physical memory manager. */
        pmm_init();

        /* Stage-3: virtual memory manager (identity mapping + paging). */
        vmm_init();

        /*
         * PMM init 已经结束，将 g_mb2_info 从物理地址转换为虚拟地址。
         * 之后 shell 命令（mmap 等）通过 high-half 地址访问 Multiboot2 info。
         */
        g_mb2_info = (const void*)PHYS_TO_VIRT((uint32_t)(uintptr_t)g_mb2_info);

        /* 拆除 identity mapping，低地址空间留给将来的用户态进程。 */
        vmm_unmap_identity();

        /*
         * 内核堆后端选择（见 kconfig.h KCONFIG_HEAP_BACKEND）
         */
#if KCONFIG_HEAP_BACKEND == 0
        kmalloc_register_backend(heap_first_fit_get_ops());
#elif KCONFIG_HEAP_BACKEND == 1
        kmalloc_register_backend(heap_slab_get_ops());
#endif
        kmalloc_init();

        /*
         * Stage 9: VMA 子系统初始化 + 注册引导时建立的内核虚拟内存区域
         *
         * [WHY] 到这里为止，内核已经建立了以下虚拟内存映射：
         *   1. 直接映射区 [0xC0000000, direct_map_end) — vmm_init 建立
         *   2. 内核堆     [KHEAP_START, KHEAP_START + heap_size) — kmalloc_init 建立
         *   将这些区域注册到 VMA 中，使 Page Fault handler 能判断
         *   故障地址是否属于合法区域，并检查访问权限。
         *
         * [NOTE] 后端在 kconfig.h 中选择（当前仅声明接口，具体后端待下次提交）。
         *   如果 vma_rbtree_get_ops 等后端可用，取消下方注释即可启用。
         */
#if KCONFIG_VMA_BACKEND == 0
        vma_register_backend(vma_sorted_array_get_ops());
#elif KCONFIG_VMA_BACKEND == 1
        vma_register_backend(vma_rbtree_get_ops());
#elif KCONFIG_VMA_BACKEND == 2
        vma_register_backend(vma_maple_get_ops());
#endif
        vma_init();

        if (vma_is_ready()) {
            /* 注册直接映射区: 内核代码 + 数据 + 直接映射的物理内存 */
            vma_add(KERNEL_VIRT_OFFSET, vmm_direct_map_end(),
                    VMA_READ | VMA_WRITE | VMA_EXEC, "direct-map");

            /* 注册内核堆区 */
            vma_add(KHEAP_START,
                    KHEAP_START + KCONFIG_HEAP_INITIAL_PAGES * VMM_PAGE_SIZE,
                    VMA_READ | VMA_WRITE, "kheap");
        }
    }

    /* --- IRQ + keyboard + shell --- */
    pic_init();
    keyboard_init();

    /* 进入交互前展示“登录界面”。 */
    kmain_print_login();

    printk("[kbd] IRQ1 enabled. Starting shell...\n");
    __asm__ volatile ("sti");

    shell_run();
}
