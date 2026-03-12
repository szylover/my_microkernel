NASM ?= nasm
CC ?= gcc
LD ?= ld
GRUB_MKRESCUE ?= grub-mkrescue
QEMU ?= qemu-system-i386
QEMU_MEM_MB ?= 256
QEMU_GDB_PORT ?= 1234
GDB ?= gdb

# Optional: auto-regenerate compile_commands.json during builds.
# Enable with: AUTO_COMPDB=1 make iso
AUTO_COMPDB ?= 0

BUILD_DIR := build
ISO_DIR := $(BUILD_DIR)/isodir
BOOT_DIR := $(ISO_DIR)/boot
GRUB_DIR := $(BOOT_DIR)/grub

KERNEL_ELF := $(BOOT_DIR)/kernel.elf
ISO_IMAGE := $(BUILD_DIR)/kernel.iso

# Auto-discover kernel sources to avoid manually maintaining object lists.
# Note: we include one-level subdirectories (e.g. src/kernel/cmds/).
KERNEL_C_SRCS := $(wildcard src/kernel/*.c) $(wildcard src/kernel/*/*.c)
KERNEL_ASM_SRCS := $(wildcard src/kernel/*.asm) $(wildcard src/kernel/*/*.asm)

KERNEL_C_OBJS := $(patsubst src/kernel/%.c,$(BUILD_DIR)/%.o,$(KERNEL_C_SRCS))
KERNEL_ASM_OBJS := $(patsubst src/kernel/%.asm,$(BUILD_DIR)/%.o,$(KERNEL_ASM_SRCS))

OBJS := $(BUILD_DIR)/boot.o $(KERNEL_C_OBJS) $(KERNEL_ASM_OBJS)

NASMFLAGS := -f elf32
CFLAGS := -std=c11 -ffreestanding -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -O2
# Use -m32 for host gcc/clang. If you use a cross compiler (i686-elf-gcc), set CC=i686-elf-gcc
CFLAGS += -m32
CFLAGS += -Iinclude
CFLAGS += -Iinclude/arch
CFLAGS += -Iinclude/drivers
CFLAGS += -Iinclude/kernel
CFLAGS += -MMD -MP
LINKER_SCRIPT := src/boot/linker.ld
LDFLAGS := -m elf_i386 -T $(LINKER_SCRIPT)

# Debug build: keep symbols, disable optimizations (better stepping/backtraces).
DEBUG ?= 0
ifeq ($(DEBUG),1)
CFLAGS := $(filter-out -O2,$(CFLAGS))
CFLAGS += -O0 -g3 -ggdb -fno-omit-frame-pointer -fno-optimize-sibling-calls
endif

DEPS := $(OBJS:.o=.d)

.PHONY: all iso run test clean tools compdb debug gdb

ifeq ($(AUTO_COMPDB),1)
ISO_PREREQS := compile_commands.json
else
ISO_PREREQS :=
endif

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

$(BUILD_DIR)/%.o: src/kernel/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/kernel/%.asm | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

$(KERNEL_ELF): $(OBJS) $(LINKER_SCRIPT) | $(GRUB_DIR)
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

iso: $(ISO_PREREQS) $(ISO_IMAGE)

run: $(ISO_IMAGE) | tools
	$(QEMU) -m $(QEMU_MEM_MB) -cdrom $(ISO_IMAGE) -serial stdio -no-reboot

# Start QEMU and wait for GDB to attach.
# In another terminal, run: `make gdb`.
debug: $(ISO_IMAGE) | tools
	$(QEMU) -m $(QEMU_MEM_MB) -cdrom $(ISO_IMAGE) -serial stdio -no-reboot -S -gdb tcp::$(QEMU_GDB_PORT)

# Launch GDB and attach to QEMU's gdbstub.
# Usage: `make debug` (terminal A), then `make gdb` (terminal B).
gdb: $(KERNEL_ELF) | tools
	$(GDB) -q -x tools/kernel.gdb

test: | tools
	bash tests/test.sh

# Generate compile_commands.json for clangd.
# Requires `bear` (https://github.com/rizsotto/Bear).
compdb:
	@command -v bear >/dev/null 2>&1 || (echo "Missing tool: bear" && echo "Install: sudo apt update && sudo apt install -y bear" && exit 2)
	@echo "[compdb] generating compile_commands.json via bear"
	@bear -- $(MAKE) -B $(KERNEL_ELF)
	@echo "[compdb] done: compile_commands.json"

# When AUTO_COMPDB=1, regenerate compdb as part of `make iso`.
# Use AUTO_COMPDB=0 in the nested make to avoid infinite recursion.
compile_commands.json: $(KERNEL_C_SRCS) $(KERNEL_ASM_SRCS) src/boot/boot.asm Makefile $(LINKER_SCRIPT)
	@command -v bear >/dev/null 2>&1 || (echo "Missing tool: bear" && echo "Install: sudo apt update && sudo apt install -y bear" && exit 2)
	@echo "[compdb] AUTO_COMPDB=1 -> updating compile_commands.json"
	@bear -- $(MAKE) -B $(KERNEL_ELF) AUTO_COMPDB=0

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
