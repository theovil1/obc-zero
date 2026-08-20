# OBC-Zero build.
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

CROSS   := riscv64-unknown-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
GDB     := $(CROSS)gdb

QEMU    := qemu-system-riscv32
MACHINE := sifive_e

BUILD   := build
TARGET  := $(BUILD)/obc.elf
LDSCRIPT := emu/sifive_e.ld

# Build identity. Reported on the boot banner so that a captured serial log can
# always be traced back to the exact tree that produced it.
GIT_HASH  := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo nogit)
GIT_DIRTY := $(shell test -n "$$(git status --porcelain 2>/dev/null)" && echo -dirty)
BUILD_HASH := $(GIT_HASH)$(GIT_DIRTY)

SRC_C := flight/core/main.c \
         flight/hal/uart.c
SRC_S := flight/boot/start.S

OBJ := $(SRC_C:%.c=$(BUILD)/%.o) $(SRC_S:%.S=$(BUILD)/%.o)
DEP := $(OBJ:.o=.d)

# rv32imac / ilp32 matches the ISA declared in README.md.
#
# _zicsr is appended because CSR access became a separate extension when the
# ISA spec was split; binutils 2.45 rejects csrw under a bare `imac` string.
# This adds no instructions the hardware did not already have, it only names
# them. See docs/adr/0001-target-platform.md.
#
# medany is required: medlow cannot address 0x80000000, which is out of range
# of its signed 32-bit absolute addressing.
ARCHFLAGS := -march=rv32imac_zicsr -mabi=ilp32 -mcmodel=medany

CFLAGS := $(ARCHFLAGS) \
          -std=c11 -ffreestanding -fno-builtin \
          -Wall -Wextra -Werror \
          -Wundef -Wshadow -Wconversion -Wsign-conversion \
          -Os -g3 \
          -ffunction-sections -fdata-sections \
          -Iflight \
          -DOBC_BUILD_HASH='"$(BUILD_HASH)"' \
          -MMD -MP

ASFLAGS := $(ARCHFLAGS) -g3 -Iflight

LDFLAGS := $(ARCHFLAGS) -T $(LDSCRIPT) \
           -nostdlib -nostartfiles \
           -Wl,--gc-sections \
           -Wl,-Map=$(BUILD)/obc.map \
           -Wl,--no-warn-rwx-segments

.PHONY: all build run test gdb size clean

all: build

build: $(TARGET) size

$(TARGET): $(OBJ) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(OBJ) -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

size: $(TARGET)
	@$(SIZE) $(TARGET)

# Interactive boot. Leave with Ctrl-A then X.
run: $(TARGET)
	$(QEMU) -machine $(MACHINE) -nographic -kernel $(TARGET)

# M0 smoke test: the image boots and identifies itself. Nothing more is in
# scope at this milestone; the real harness arrives at M9.
test: $(TARGET)
	@echo "smoke: boot banner"
	@out=$$(timeout 10s $(QEMU) -machine $(MACHINE) -display none \
	          -serial stdio -kernel $(TARGET) 2>&1 || true); \
	echo "$$out"; \
	echo "$$out" | grep -q "boot   : ok" || { echo "FAIL: no boot confirmation"; exit 1; }; \
	echo "$$out" | grep -q "build  : $(BUILD_HASH)" || { echo "FAIL: build hash mismatch"; exit 1; }; \
	echo "PASS"

# Halted at reset with a gdbstub on :1234. In another shell:
#   $(GDB) $(TARGET) -ex 'target remote :1234'
gdb: $(TARGET)
	$(QEMU) -machine $(MACHINE) -nographic -kernel $(TARGET) -s -S

clean:
	rm -rf $(BUILD)

-include $(DEP)
