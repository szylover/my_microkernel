OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS
{
    /* Link at 1 MiB (physical & virtual for now). */
    . = 1M;

    /*
     * Export kernel physical bounds for early memory managers.
     *
     * We are still running without paging (identity mapping), so these
     * addresses are both physical and virtual.
     */
    __kernel_phys_start = .;

    /* Put the Multiboot2 header first and inside a loadable section. */
    .text ALIGN(4096) :
    {
        KEEP(*(.multiboot_header))
        *(.text .text.*)
    }

    .rodata ALIGN(4096) :
    {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4096) :
    {
        *(.data .data.*)
    }

    .bss ALIGN(4096) :
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    __kernel_phys_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
        *(.gnu.*)
    }
}