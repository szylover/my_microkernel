OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS
{
    /* Link at 1 MiB (physical & virtual for now). */
    . = 1M;

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

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
        *(.gnu.*)
    }
}