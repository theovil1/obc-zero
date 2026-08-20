# OBC-Zero build.
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

CROSS   := riscv64-unknown-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size

# No riscv64-unknown-elf-gdb is packaged: neither gcc-riscv64-unknown-elf nor
# binutils-riscv64-unknown-elf ships one, and no apt package provides it.
# gdb-multiarch carries the RISC-V targets despite reporting itself as
# "configured as x86_64-linux-gnu", which refers to its host, not its targets.
GDB     := gdb-multiarch

# The architecture must be set before connecting: on an already-attached target
# GDB auto-detects and falls back to its host architecture.
GDBFLAGS := -ex 'set architecture riscv:rv32' -ex 'target remote localhost:1234'

QEMU    := qemu-system-riscv32
MACHINE := sifive_e

BUILD   := build
TARGET  := $(BUILD)/obc.elf
LDSCRIPT := emu/sifive_e.ld

# Build identity, stamped into the boot banner. This is a build artefact, not a
# reference: a -dirty banner is fine on a working build. Reference measurements
# are taken by `make measure`, which refuses to run on a dirty tree at all.
BUILD_HASH := $(shell git describe --always --dirty 2>/dev/null || echo nogit)

# The exact string the host waits for to know a run has reached its end state.
# Must match the sentinel emitted by flight/core/main.c.
SENTINEL := boot   : ok

# Upper bound on how long the host waits for the sentinel before giving up.
# Reaching it is a failure, not a normal end of run.
RUN_TIMEOUT_S := 10

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

.PHONY: all build run test measure gdb attach size clean

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
#
# The run ends when the host sees the sentinel, not when a timeout expires.
# sifive_e has no test finisher, so the firmware cannot exit QEMU by itself and
# the host has to stop it. The timeout below is the failure path, not the
# normal one. M9 replaces this shell loop with harness/runner/.
test: $(TARGET)
	@echo "smoke: boot banner"
	@rm -f $(BUILD)/serial.log && touch $(BUILD)/serial.log
	@$(QEMU) -machine $(MACHINE) -display none -serial file:$(BUILD)/serial.log \
	    -kernel $(TARGET) & \
	 qpid=$$!; \
	 found=0; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 20 ))); do \
	   if grep -qF "$(SENTINEL)" $(BUILD)/serial.log 2>/dev/null; then found=1; break; fi; \
	   kill -0 $$qpid 2>/dev/null || break; \
	   sleep 0.05; \
	 done; \
	 kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	 cat $(BUILD)/serial.log; \
	 test $$found -eq 1 || { echo "FAIL: sentinel not seen within $(RUN_TIMEOUT_S)s"; exit 1; }; \
	 grep -qF "build  : $(BUILD_HASH)" $(BUILD)/serial.log \
	   || { echo "FAIL: build hash mismatch"; exit 1; }; \
	 echo "PASS"

# Reference measurement. Refuses to run on a dirty tree: a -dirty hash is not
# reproducible by anyone, and reproducibility is the whole claim of this
# project. Commit first, measure second, record the result in a later commit
# that names the commit it measured.
measure:
	@test -z "$$(git status --porcelain 2>/dev/null)" \
	  || { echo "REFUSED: working tree is dirty."; \
	       echo "A reference measurement must name a commit others can check out."; \
	       git status --short; exit 1; }
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory $(TARGET) >/dev/null
	@echo "commit : $$(git rev-parse HEAD)"
	@echo "describe: $$(git describe --always --dirty)"
	@echo "toolchain: $$($(CC) -dumpversion), $$($(QEMU) --version | head -1)"
	@echo
	@$(SIZE) $(TARGET)
	@echo
	@$(MAKE) --no-print-directory test

# Halted at reset with a gdbstub on :1234. Start this first, then attach from
# another shell with `make attach`. Connecting before QEMU is listening reports
# a connection timeout rather than a refusal, which reads like a GDB fault and
# is not one.
gdb: $(TARGET)
	$(QEMU) -machine $(MACHINE) -nographic -kernel $(TARGET) -s -S

# Attach to a `make gdb` already running in another shell.
attach: $(TARGET)
	$(GDB) $(TARGET) $(GDBFLAGS)

clean:
	rm -rf $(BUILD)

-include $(DEP)
