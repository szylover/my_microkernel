set pagination off
set confirm off
set disassembly-flavor intel
set architecture i386

# Load symbols from the linked kernel ELF.
symbol-file build/isodir/boot/kernel.elf

# Connect to QEMU gdbstub (started by: make debug)
target remote :1234

# Common useful breakpoints
break kmain

# Start running (you can remove/disable this line if you prefer manual control)
continue
