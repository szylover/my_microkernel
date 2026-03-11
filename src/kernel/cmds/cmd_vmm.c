#include "cmd.h"

#include <stdint.h>
#include <stddef.h>

#include "vmm.h"
#include "pmm.h"
#include "printk.h"

/* ============================================================================
 * 小工具（复用自 cmd_pmm.c 的风格）
 * ============================================================================ */

static int streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int parse_u32(const char* s, uint32_t* out) {
    if (!s || !*s || !out) return 0;
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s) return 0;
        while (*s) {
            int hv = hex_val(*s++);
            if (hv < 0) return 0;
            v = (v << 4) | (uint32_t)hv;
        }
        *out = v;
        return 1;
    }
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    *out = v;
    return 1;
}

/* ============================================================================
 * PTE/PDE flags 解码（人类可读字符串）
 * ============================================================================ */

static void print_flags(uint32_t entry) {
    if (!(entry & 0x001u)) {
        printk("NOT PRESENT");
        return;
    }
    printk("P");                                     /* Present */
    printk("%s", (entry & 0x002u) ? " RW" : " RO");  /* Read/Write or Read-Only */
    printk("%s", (entry & 0x004u) ? " U"  : " S");   /* User or Supervisor */
    if (entry & 0x020u) printk(" A");                 /* Accessed */
    if (entry & 0x040u) printk(" D");                 /* Dirty */
}

/* ============================================================================
 * 子命令实现
 * ============================================================================ */

static void cmd_vmm_usage(void) {
    printk("Usage:\n");
    printk("  vmm state              Show VMM status\n");
    printk("  vmm lookup <addr>      Translate virtual -> physical\n");
    printk("  vmm pd [start] [count] Dump PDE entries\n");
    printk("  vmm pt <pd_idx>        Dump PTE entries for a PDE\n");
    printk("  vmm map <virt> <phys>  Map a virtual page\n");
    printk("  vmm unmap <virt>       Unmap a virtual page\n");
    printk("  vmm fault              Trigger a test page fault\n");
    printk("Notes:\n");
    printk("  Addresses can be decimal or hex (0x...)\n");
}

static int cmd_vmm_state(void) {
    if (!vmm_is_ready()) {
        printk("VMM: not initialized (paging off)\n");
        return 0;
    }

    printk("VMM: paging enabled\n");

    /* Count how many PDE entries are present */
    unsigned pd_present = 0;
    for (unsigned i = 0; i < VMM_ENTRIES_PER_PD; i++) {
        if (vmm_get_pde(i) & PDE_PRESENT) {
            pd_present++;
        }
    }
    printk("VMM: %u / %u PDE entries present\n", pd_present, (unsigned)VMM_ENTRIES_PER_PD);
    printk("VMM: each PDE covers 4 MiB (%u MiB mapped)\n", pd_present * 4u);
    return 0;
}

static int cmd_vmm_lookup(int argc, char** argv) {
    if (argc < 3) {
        printk("vmm lookup: missing <addr>\n");
        return 0;
    }

    uint32_t virt = 0;
    if (!parse_u32(argv[2], &virt)) {
        printk("vmm lookup: invalid addr: %s\n", argv[2]);
        return 0;
    }

    /* Align down to page boundary for lookup */
    uint32_t page_virt = virt & ~0xFFFu;

    uint32_t phys = 0;
    if (vmm_get_physical(page_virt, &phys) == 0) {
        uint32_t pte = vmm_get_pte(page_virt);
        printk("virt 0x%08x -> phys 0x%08x  [", page_virt, phys);
        print_flags(pte);
        printk("]\n");
    } else {
        printk("virt 0x%08x -> NOT MAPPED\n", page_virt);
    }
    return 0;
}

static int cmd_vmm_pd(int argc, char** argv) {
    uint32_t start = 0;
    uint32_t count = 16;

    if (argc >= 3) {
        if (!parse_u32(argv[2], &start)) {
            printk("vmm pd: invalid start: %s\n", argv[2]);
            return 0;
        }
    }
    if (argc >= 4) {
        if (!parse_u32(argv[3], &count) || count == 0) {
            printk("vmm pd: invalid count: %s\n", argv[3]);
            return 0;
        }
    }
    if (count > 128u) {
        printk("vmm pd: count capped to 128\n");
        count = 128u;
    }
    if (start >= VMM_ENTRIES_PER_PD) {
        printk("vmm pd: start out of range (%u >= %u)\n", (unsigned)start, (unsigned)VMM_ENTRIES_PER_PD);
        return 0;
    }

    uint32_t end = start + count;
    if (end > VMM_ENTRIES_PER_PD) end = VMM_ENTRIES_PER_PD;

    printk("PD[%u..%u)  (each PDE covers 4 MiB)\n", (unsigned)start, (unsigned)end);
    for (uint32_t i = start; i < end; i++) {
        uint32_t pde = vmm_get_pde(i);
        if (!(pde & PDE_PRESENT)) continue;

        uint32_t virt_base = i * 4u * 1024u * 1024u;
        uint32_t pt_phys = pde & ~0xFFFu;
        printk("  PD[%3u] virt=0x%08x pt=0x%08x [", (unsigned)i, virt_base, pt_phys);
        print_flags(pde);
        printk("]\n");
    }
    return 0;
}

static int cmd_vmm_pt(int argc, char** argv) {
    if (argc < 3) {
        printk("vmm pt: missing <pd_idx>\n");
        return 0;
    }

    uint32_t pd_idx = 0;
    if (!parse_u32(argv[2], &pd_idx)) {
        printk("vmm pt: invalid pd_idx: %s\n", argv[2]);
        return 0;
    }
    if (pd_idx >= VMM_ENTRIES_PER_PD) {
        printk("vmm pt: pd_idx out of range\n");
        return 0;
    }

    uint32_t pde = vmm_get_pde(pd_idx);
    if (!(pde & PDE_PRESENT)) {
        printk("PD[%u]: NOT PRESENT\n", (unsigned)pd_idx);
        return 0;
    }

    uint32_t virt_base = pd_idx * 4u * 1024u * 1024u;
    printk("PD[%u] page table (virt base 0x%08x):\n", (unsigned)pd_idx, virt_base);

    /* Only show present entries (1024 lines would be too much) */
    unsigned shown = 0;
    for (unsigned pti = 0; pti < VMM_ENTRIES_PER_PT; pti++) {
        uint32_t virt = virt_base + pti * VMM_PAGE_SIZE;
        uint32_t pte = vmm_get_pte(virt);
        if (!(pte & PTE_PRESENT)) continue;

        uint32_t phys = pte & ~0xFFFu;
        printk("  PT[%3u] virt=0x%08x -> phys=0x%08x [", pti, virt, phys);
        print_flags(pte);
        printk("]\n");
        shown++;

        /* 防止串口输出爆掉 */
        if (shown >= 64) {
            printk("  ... (truncated, %u+ entries)\n", shown);
            break;
        }
    }

    if (shown == 0) {
        printk("  (all 1024 PTEs are empty)\n");
    }
    return 0;
}

static int cmd_vmm_map(int argc, char** argv) {
    if (argc < 4) {
        printk("vmm map: usage: vmm map <virt> <phys>\n");
        return 0;
    }

    uint32_t virt = 0, phys = 0;
    if (!parse_u32(argv[2], &virt)) {
        printk("vmm map: invalid virt: %s\n", argv[2]);
        return 0;
    }
    if (!parse_u32(argv[3], &phys)) {
        printk("vmm map: invalid phys: %s\n", argv[3]);
        return 0;
    }

    /* 对齐检查 */
    if ((virt & 0xFFFu) || (phys & 0xFFFu)) {
        printk("vmm map: addresses must be 4KiB aligned\n");
        return 0;
    }

    int ret = vmm_map_page(virt, phys, PTE_PRESENT | PTE_WRITABLE);
    if (ret == 0) {
        printk("vmm map: 0x%08x -> 0x%08x OK\n", virt, phys);
    } else {
        printk("vmm map: failed (PMM OOM?)\n");
    }
    return 0;
}

static int cmd_vmm_unmap(int argc, char** argv) {
    if (argc < 3) {
        printk("vmm unmap: missing <virt>\n");
        return 0;
    }

    uint32_t virt = 0;
    if (!parse_u32(argv[2], &virt)) {
        printk("vmm unmap: invalid virt: %s\n", argv[2]);
        return 0;
    }
    if (virt & 0xFFFu) {
        printk("vmm unmap: address must be 4KiB aligned\n");
        return 0;
    }

    vmm_unmap_page(virt);
    printk("vmm unmap: 0x%08x OK\n", virt);
    return 0;
}

/*
 * vmm fault — 主动触发一个 Page Fault (用于测试 #PF handler)
 *
 * [WHY]
 *   读取一个绝对不会被映射的虚拟地址（0xDEAD0000），验证：
 *   1. CPU 能正确触发 #PF (vector 14)
 *   2. 我们的 page fault handler 能读到 CR2 并打出详细信息
 *   3. Kernel panic 能停下来而非 triple fault / 无限重启
 */
static int cmd_vmm_fault(void) {
    printk("vmm fault: about to read unmapped address 0xDEAD0000...\n");
    /*
     * volatile 防止编译器优化掉这个"无用"的读操作。
     * 这一行执行后，CPU 将：
     *   1. 查页目录 PD[0xDEA >> 2] = PD[886] → 大概率 not present
     *   2. 触发 #PF，压入 error code，跳到 isr14
     *   3. 我们的 page_fault_handler 读 CR2 = 0xDEAD0000 并打印
     */
    volatile uint32_t* bad_ptr = (volatile uint32_t*)0xDEAD0000u;
    (void)*bad_ptr;

    /* 不会到这里（#PF handler 会 halt） */
    return 0;
}

/* ============================================================================
 * 命令入口
 * ============================================================================ */

static int cmd_vmm_main(int argc, char** argv) {
    if (!vmm_is_ready()) {
        printk("VMM: not initialized\n");
        return 0;
    }

    if (argc < 2) {
        cmd_vmm_usage();
        return 0;
    }

    if (streq(argv[1], "state"))  return cmd_vmm_state();
    if (streq(argv[1], "lookup")) return cmd_vmm_lookup(argc, argv);
    if (streq(argv[1], "pd"))     return cmd_vmm_pd(argc, argv);
    if (streq(argv[1], "pt"))     return cmd_vmm_pt(argc, argv);
    if (streq(argv[1], "map"))    return cmd_vmm_map(argc, argv);
    if (streq(argv[1], "unmap"))  return cmd_vmm_unmap(argc, argv);
    if (streq(argv[1], "fault"))  return cmd_vmm_fault();

    printk("vmm: unknown subcommand: %s\n", argv[1]);
    cmd_vmm_usage();
    return 0;
}

const cmd_t cmd_vmm = {
    .name = "vmm",
    .help = "Virtual memory manager (paging) inspection & test",
    .fn = cmd_vmm_main,
};
