OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

KERNEL_VIRT_BASE = 0xC0000000;

SECTIONS
{
    /*
     * [WHY] Physical load address = 1 MiB.
     * GRUB loads the kernel at the physical address specified in ELF p_paddr.
     * The kernel image must start above 1 MiB (below is BIOS/legacy area).
     */
    . = 1M;

    __kernel_phys_start = .;

    /*
     * Boot sections: linked and loaded at physical address (< 0xC0000000).
     *
     * [WHY] The Multiboot2 header must be in the first 32 KiB of the OS image.
     * Early boot code (_start) runs before paging is enabled, so it must be
     * addressable via physical addresses.
     */
    .boot ALIGN(4096) :
    {
        KEEP(*(.multiboot_header))
        *(.boot .boot.*)
    }

    .boot_bss ALIGN(4096) (NOLOAD) :
    {
        *(.boot_bss .boot_bss.*)
    }

    /*
     * ========================================================================
     * Switch to high-half virtual address space.
     * ========================================================================
     *
     * [WHY] All kernel code/data below is linked at virtual 0xC0000000+,
     * but physically loaded right after the boot sections (via AT()).
     * This is the "high-half kernel" layout:
     *   - User space:  0x00000000 - 0xBFFFFFFF (3 GiB)
     *   - Kernel:      0xC0000000 - 0xFFFFFFFF (1 GiB)
     *
     * [CPU STATE] After paging is enabled with high-half mapping:
     *   virt 0xC0100000+ → phys 0x00100000+ (kernel image)
     */
    . += KERNEL_VIRT_BASE;

    __kernel_virt_start = .;

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VIRT_BASE)
    {
        *(.text .text.*)
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VIRT_BASE)
    {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VIRT_BASE)
    {
        *(.data .data.*)
    }

    .bss ALIGN(4096) (NOLOAD) : AT(ADDR(.bss) - KERNEL_VIRT_BASE)
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    __kernel_virt_end = .;

    /*
     * Physical end address of the kernel image.
     * Used by PMM to reserve the kernel region in the bitmap.
     */
    __kernel_phys_end = __kernel_virt_end - KERNEL_VIRT_BASE;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
        *(.gnu.*)
    }
}