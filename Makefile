NASM ?= nasm
CC ?= gcc
LD ?= ld
GRUB_MKRESCUE ?= grub-mkrescue
QEMU ?= qemu-system-i386

BUILD_DIR := build
ISO_DIR := $(BUILD_DIR)/isodir
BOOT_DIR := $(ISO_DIR)/boot
GRUB_DIR := $(BOOT_DIR)/grub

KERNEL_ELF := $(BOOT_DIR)/kernel.elf
ISO_IMAGE := $(BUILD_DIR)/kernel.iso

OBJS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/kmain.o $(BUILD_DIR)/serial.o $(BUILD_DIR)/printk.o $(BUILD_DIR)/gdt.o $(BUILD_DIR)/gdt_flush.o $(BUILD_DIR)/idt.o $(BUILD_DIR)/isr_stubs.o

NASMFLAGS := -f elf32
CFLAGS := -std=c11 -ffreestanding -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -O2
# Use -m32 for host gcc/clang. If you use a cross compiler (i686-elf-gcc), set CC=i686-elf-gcc
CFLAGS += -m32
CFLAGS += -Iinclude
LDFLAGS := -m elf_i386 -T linker.ld

.PHONY: all iso run clean tools

all: iso

tools:
	@command -v $(NASM) >/dev/null || (echo "Missing tool: $(NASM)" && exit 1)
	@command -v $(CC) >/dev/null || (echo "Missing tool: $(CC)" && exit 1)
	@command -v $(LD) >/dev/null || (echo "Missing tool: $(LD)" && exit 1)
	@command -v $(GRUB_MKRESCUE) >/dev/null || (echo "Missing tool: $(GRUB_MKRESCUE)" && exit 1)
	@command -v $(QEMU) >/dev/null || (echo "Missing tool: $(QEMU)" && exit 1)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot.o: src/boot/boot.asm | $(BUILD_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

$(BUILD_DIR)/kmain.o: src/kernel/kmain.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/serial.o: src/kernel/serial.c include/serial.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/printk.o: src/kernel/printk.c include/printk.h include/serial.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/gdt.o: src/kernel/gdt.c include/gdt.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/gdt_flush.o: src/kernel/gdt_flush.asm | $(BUILD_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

$(BUILD_DIR)/idt.o: src/kernel/idt.c include/idt.h include/gdt.h include/serial.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/isr_stubs.o: src/kernel/isr_stubs.asm | $(BUILD_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

$(KERNEL_ELF): $(OBJS) linker.ld | $(GRUB_DIR)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(GRUB_DIR): | $(BUILD_DIR)
	mkdir -p $(GRUB_DIR)

$(GRUB_DIR)/grub.cfg: | $(GRUB_DIR)
	@printf '%s\n' \
		"set timeout=0" \
		"set default=0" \
		"menuentry 'my_microkernel' {" \
		"  multiboot2 /boot/kernel.elf" \
		"  boot" \
		"}" > $@

$(ISO_IMAGE): $(KERNEL_ELF) $(GRUB_DIR)/grub.cfg | tools
	$(GRUB_MKRESCUE) -o $@ $(ISO_DIR)

iso: $(ISO_IMAGE)

run: $(ISO_IMAGE) | tools
	$(QEMU) -cdrom $(ISO_IMAGE) -serial stdio -no-reboot

clean:
	rm -rf $(BUILD_DIR)