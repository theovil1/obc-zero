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

# Deterministic execution. shift=N means one instruction consumes 2^N ns of
# virtual time, so N sets the emulated CPU speed.
#
# Without icount, mcycle and minstret report host behaviour: the same workload
# varies by 12 % between runs, per-task budgets cannot be enforced, and a seeded
# fault injector lands on a different instruction every time.
#
# shift=6 gives 64 ns per instruction, an emulated 15.62 MHz against the FE310's
# real 16 MHz. shift=0 would model a 1 GHz core driving a 32768 Hz timer, a ratio
# 64x away from the target hardware, which would distort every execution budget
# calibrated against it. It is also 32x slower in host time for no benefit.
#
# Applied to every invocation, not only to campaigns: a test that runs under
# different execution semantics from the campaign is not testing the campaign.
# See docs/adr/0002-time-domains.md.
ICOUNT  := -icount shift=6

BUILD   := build
TARGET  := $(BUILD)/obc.elf
LDSCRIPT := emu/sifive_e.ld

# Build identity, stamped into the boot banner. This is a build artefact, not a
# reference: a -dirty banner is fine on a working build. Reference measurements
# are taken by `make measure`, which refuses to run on a dirty tree at all.
#
# Padded to a fixed width so that image size never depends on the hash. Moving
# the hash out of the mergeable string pool removed the dependency on its
# *content*; this removes the dependency on its *length*, which is what made a
# -dirty build 8 bytes larger than a clean one and would otherwise make
# `size-check` fail spuriously on any uncommitted work.
BUILD_HASH_WIDTH := 20
BUILD_HASH_RAW := $(shell git describe --always --dirty 2>/dev/null || echo nogit)
BUILD_HASH := $(shell printf '%-$(BUILD_HASH_WIDTH).$(BUILD_HASH_WIDTH)s' '$(BUILD_HASH_RAW)')

# The exact string the host waits for to know a run has reached its end state.
# Must match the sentinel emitted by flight/core/main.c.
SENTINEL := boot   : ok

# Upper bound on how long the host waits for the sentinel before giving up.
# Reaching it is a failure, not a normal end of run.
RUN_TIMEOUT_S := 10

# MTIME_SRC selects which machine-timer implementation is linked. The default is
# the flight one; test-carry-broken swaps in a deliberately naive reader to prove
# the carry test detects something. Must be defined before SRC_C uses it.
MTIME_SRC ?= flight/hal/mtime.c

# SCHED_SRC selects which executive is linked. The default is the flight one;
# test-sched-broken swaps in a copy that drops one dispatch, to prove the
# conformance and ordering assertions detect something.
SCHED_SRC ?= flight/core/sched.c

# TASKS_SRC selects the task table. test-sched-overrun swaps in one whose budget
# is far below what its task costs.
TASKS_SRC ?= flight/core/tasks.c

SRC_C := flight/core/main.c \
         flight/core/mode.c \
         $(SCHED_SRC) \
         $(TASKS_SRC) \
         flight/core/fault.c \
         $(MTIME_SRC) \
         flight/hal/uart.c
SRC_S := flight/boot/start.S \
         flight/boot/trap.S

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
          -DOBC_BUILD_HASH='"$(BUILD_HASH)"'  $(EXTRA_CFLAGS) \
          -MMD -MP

ASFLAGS := $(ARCHFLAGS) -g3 -Iflight

LDFLAGS := $(ARCHFLAGS) -T $(LDSCRIPT) \
           -nostdlib -nostartfiles \
           -Wl,--gc-sections \
           -Wl,-Map=$(BUILD)/obc.map \
           -Wl,--no-warn-rwx-segments

.PHONY: all build run deps-check lint test-safe safe-one safe-clock test test-sched test-sched-repro test-sched-broken sched-expect-reject test-sched-overrun sched-expect-overrun test-trap trap-one test-record record-one test-stability test-poisoned test-carry carry-one test-carry-broken test-carry-expect-fault measure gdb attach size size-check size-accept clean

all: build

build: $(TARGET) size

# Everything depends on the Makefile: CFLAGS, the build hash and the link flags
# all live here, so an edit to this file changes the image. Omitting this
# dependency let a stale ELF survive a flag change, and the size reference was
# then captured from a binary that no longer matched the source.
$(TARGET): $(OBJ) $(LDSCRIPT) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(OBJ) -o $@

$(BUILD)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S Makefile
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

size: $(TARGET)
	@$(SIZE) $(TARGET)

# --- Size reference -----------------------------------------------------------
#
# docs/BUDGET.md is a register of claims about memory consumption. A claim is
# only worth something if a figure cannot move without someone noticing, so the
# per-section sizes are pinned in a versioned reference and compared on every
# measurement. Any drift fails the build.
#
# The point is not to freeze the image. It is to make every size change a
# deliberate act: run `make size-accept` and say why in the commit message.
#
# Debug sections are excluded on purpose. They are not loaded, they do not
# consume flash or RAM, and they move with compiler flags that have nothing to
# do with the budget.
SIZE_SECTIONS := \.init|\.text|\.rodata|\.data|\.bss|\.stack
SIZE_REF      := docs/size-reference.txt
SIZE_ACTUAL   := $(BUILD)/size-actual.txt

# The toolchain version is part of the reference. A compiler upgrade changes
# code size for reasons that have nothing to do with this project, and without
# this header the result is a drift failure with no explanation in it. With it,
# the diff says "gcc moved" on its first line.
$(SIZE_ACTUAL): $(TARGET)
	@mkdir -p $(dir $@)
	@{ echo "# gcc      $$($(CC) -dumpversion)"; \
	   echo "# binutils $$($(CROSS)ld --version | head -1 | grep -oE '[0-9]+\.[0-9]+[.0-9]*' | head -1)"; \
	   $(SIZE) -A $(TARGET) \
	     | grep -E '^($(SIZE_SECTIONS))[[:space:]]' \
	     | awk '{ printf "%-10s %6d\n", $$1, $$2 }' \
	     | LC_ALL=C sort; } > $@

size-check: $(SIZE_ACTUAL)
	@test -f $(SIZE_REF) || { \
	  echo "no size reference at $(SIZE_REF)."; \
	  echo "Create it with 'make size-accept' and commit it."; exit 1; }
	@if diff -u $(SIZE_REF) $(SIZE_ACTUAL) > $(BUILD)/size.diff 2>&1; then \
	  echo "size: matches $(SIZE_REF)"; \
	else \
	  echo "FAIL: section sizes drifted from $(SIZE_REF)"; \
	  echo; sed '1,2d' $(BUILD)/size.diff; echo; \
	  echo "If this change is intended, run 'make size-accept' and explain it"; \
	  echo "in the commit message. If it is not, you have just found something."; \
	  exit 1; \
	fi

size-accept: $(SIZE_ACTUAL)
	@cp $(SIZE_ACTUAL) $(SIZE_REF)
	@echo "size reference updated:"
	@cat $(SIZE_REF)
	@echo
	@echo "This is a deliberate change. Say why in the commit message."

# Interactive boot. Leave with Ctrl-A then X.
run: $(TARGET)
	$(QEMU) -machine $(MACHINE) $(ICOUNT) -nographic -kernel $(TARGET)

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
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none -serial file:$(BUILD)/serial.log \
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

# --- Forced mtime carry -------------------------------------------------------
#
# The 64-bit read race cannot be waited for: a carry happens once every 36 hours
# at 32768 Hz, and under -icount the run is deterministic, so it either meets the
# window or never does. Never would mean a permanently green test on a broken
# clock. The carry is therefore forced through the gdbstub.
#
# Located from the CARRY-INJECT marker rather than hardcoded, so that editing
# either implementation cannot silently move the injection point off the read.
CARRY_LINE = $(MTIME_SRC):$(shell grep -n 'CARRY-INJECT' $(MTIME_SRC) | cut -d: -f1)

# $(1) = expected sentinel, $(2) = high word to cross from, $(3) = failure label
define run_carry
	@rm -f $(BUILD)/serial.log && touch $(BUILD)/serial.log
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    -serial file:$(BUILD)/serial.log -kernel $(TARGET) -s -S & \
	 qpid=$$!; \
	 sleep 1; \
	 timeout 30 $(GDB) $(TARGET) -batch -ex 'set $$carry_line = "$(CARRY_LINE)"' \
	   -ex 'set $$carry_hi = $(2)' -x harness/faults/carry.gdb > $(BUILD)/carry.log 2>&1; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 20 ))); do \
	   grep -qE 'boot   : (ok|FAULT)' $(BUILD)/serial.log 2>/dev/null && break; \
	   kill -0 $$qpid 2>/dev/null || break; \
	   sleep 0.05; \
	 done; \
	 kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	 grep -E 'tick|boot' $(BUILD)/serial.log; \
	 grep -qF 'CARRY-INJECTED' $(BUILD)/carry.log \
	   || { echo "FAIL: the injection never happened, so this test proved nothing"; \
	        cat $(BUILD)/carry.log; exit 1; }; \
	 grep -qF "$(1)" $(BUILD)/serial.log \
	   || { echo "FAIL: $(3)"; cat $(BUILD)/carry.log; exit 1; }
endef

# High words to cross from. Not 0..9: a run of consecutive values would never
# reach a byte, half-word or sign boundary in the high word itself.
CARRY_HI_VALUES := 0 1 2 255 256 65535 65536 2147483647 2147483648 4294967294

# The flight reader must survive a carry landing between its two reads.
test-carry: $(TARGET)
	@echo "carry: forced mid-read carry, correct implementation"
	@for hi in $(CARRY_HI_VALUES); do \
	   printf "  hi=%-12s " $$hi; \
	   $(MAKE) --no-print-directory CARRY_HI=$$hi carry-one >/dev/null 2>&1 \
	     && echo "survived" \
	     || { echo "FAILED"; $(MAKE) --no-print-directory CARRY_HI=$$hi carry-one; exit 1; }; \
	 done
	@echo "PASS (10 crossings, each in its own run)"

# Internal. Same run, opposite expectation: the reader under test must be caught.
# Not called directly — it needs MTIME_SRC set, which is what test-carry-broken
# arranges.
carry-one: $(TARGET)
	$(call run_carry,boot   : ok,$(CARRY_HI),the flight reader did not survive a forced carry)

test-carry-expect-fault: $(TARGET)
	$(call run_carry,boot   : FAULT,0,the naive reader survived — the test detects nothing)

# The naive reader must NOT survive the forced carry. If this passes, the test
# is measuring nothing and every green run above is worthless.
#
# The whole run happens inside the sub-make, not just the build. Overriding
# MTIME_SRC for the build alone left the parent still pointing the debugger at
# the flight source, so the breakpoint resolved against a file absent from the
# binary and the run hung instead of failing.
test-carry-broken:
	@echo "carry: forced mid-read carry, deliberately naive implementation"
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory MTIME_SRC=harness/broken/mtime_naive.c \
	    test-carry-expect-fault
	@echo "PASS (the broken build was correctly rejected)"
	@$(MAKE) --no-print-directory clean >/dev/null

# --- Read stability -----------------------------------------------------------
#
# A property distinct from carry handling: a reader that occasionally slips
# would not be caught by any number of forced carries. One million reads, each
# subject to the same bounded-progression check.
#
# Reported as reads and ticks, never converted to a duration. Under -icount the
# relationship between the two and wall-clock time is a build parameter, so
# "the clock was stable for X seconds" would be a claim this test cannot make.
STABILITY_READS ?= 1000000

test-stability:
	@echo "stability: $(STABILITY_READS) reads under bounded-progression check"
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory \
	    EXTRA_CFLAGS='-DTICK_CHECK_READS=$(STABILITY_READS)u' \
	    RUN_TIMEOUT_S=120 test
	@$(MAKE) --no-print-directory clean >/dev/null

# --- Scheduler conformance ----------------------------------------------------
#
# What is asserted is not that the tasks run. It is that the order and the
# number of executions in the window are exactly those the table dictates, and
# that they do not change between runs. See docs/adr/0003-scheduler-observability.md.
#
# The trace is read out of guest RAM through the debugger rather than printed by
# the firmware: emitting it inside a dispatch would add instructions to the very
# count being asserted.
PY := python3
TRACE_CHECK := harness/runner/trace_check.py

# $(1) = output file
define dump_trace
	@rm -f $(1) $(BUILD)/serial.log && touch $(BUILD)/serial.log
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    -serial file:$(BUILD)/serial.log -kernel $(TARGET) -s & \
	 qpid=$$!; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 20 ))); do \
	   grep -qF "$(SENTINEL)" $(BUILD)/serial.log 2>/dev/null && break; \
	   kill -0 $$qpid 2>/dev/null || break; sleep 0.05; \
	 done; \
	 grep -qF "$(SENTINEL)" $(BUILD)/serial.log \
	   || { echo "FAIL: the window never closed"; cat $(BUILD)/serial.log; \
	        kill $$qpid 2>/dev/null; exit 1; }; \
	 timeout 60 $(GDB) $(TARGET) -batch -x harness/runner/dump_trace.gdb \
	   > $(1) 2>&1; \
	 kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	 grep -qF 'DUMP-COMPLETE' $(1) \
	   || { echo "FAIL: the debugger did not finish reading guest memory"; \
	        tail -5 $(1); exit 1; }
endef

test-sched: $(TARGET)
	@echo "sched: conformance and ordering against the task table"
	$(call dump_trace,$(BUILD)/trace-1.txt)
	@$(PY) $(TRACE_CHECK) $(BUILD)/trace-1.txt

# Two runs of the same image must be indistinguishable. Under -icount any
# difference is a defect by definition, since there is no non-determinism left
# to attribute it to.
# The checker must reject a scheduler that drops a single dispatch. Without this
# every green run above is an assertion about nothing.
#
# The defect is chosen to be unremarkable: the system boots, every task runs, the
# frames are waited out, and the serial log looks correct. Only the count and the
# order betray it.
test-sched-broken:
	@echo "sched: a dropped dispatch must be rejected"
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory SCHED_SRC=harness/broken/sched_skip.c \
	    sched-expect-reject
	@echo "PASS (the broken executive was correctly rejected)"
	@$(MAKE) --no-print-directory clean >/dev/null

sched-expect-reject: $(TARGET)
	$(call dump_trace,$(BUILD)/trace-broken.txt)
	@if $(PY) $(TRACE_CHECK) $(BUILD)/trace-broken.txt > $(BUILD)/broken.out 2>&1; then \
	   echo "FAIL: the checker accepted a scheduler that drops a dispatch"; \
	   cat $(BUILD)/broken.out; exit 1; \
	 else \
	   sed 's/^/    /' $(BUILD)/broken.out; \
	 fi

# An overrun must be detected and counted, and the task must NOT have been
# prevented from running. Checking only the first half would pass on an
# implementation that killed the task — a different system substituted for this
# one without anyone saying so. See ADR 0003.
test-sched-overrun:
	@echo "sched: an overrun is counted, and the task still ran to completion"
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory TASKS_SRC=harness/broken/tasks_overrun.c \
	    sched-expect-overrun
	@echo "PASS"
	@$(MAKE) --no-print-directory clean >/dev/null

sched-expect-overrun: $(TARGET)
	$(call dump_trace,$(BUILD)/trace-over.txt)
	@$(PY) $(TRACE_CHECK) $(BUILD)/trace-over.txt > $(BUILD)/over.out 2>&1 \
	  || { echo "FAIL: conformance broke, so this is not a clean overrun test"; \
	       cat $(BUILD)/over.out; exit 1; }
	@sed 's/^/    /' $(BUILD)/over.out
	@grep -qE '^  note: .*overran' $(BUILD)/over.out \
	  || { echo "FAIL: the overrun was not detected"; exit 1; }
	@grep -qE 'order and counts match the table' $(BUILD)/over.out \
	  || { echo "FAIL: the overrunning task was prevented from running"; exit 1; }

test-sched-repro: $(TARGET)
	@echo "sched: two runs must produce identical traces and counts"
	$(call dump_trace,$(BUILD)/trace-a.txt)
	$(call dump_trace,$(BUILD)/trace-b.txt)
	@$(PY) $(TRACE_CHECK) $(BUILD)/trace-a.txt $(BUILD)/trace-b.txt

# --- Safe mode ----------------------------------------------------------------
#
# ADR 0005 requires safe mode to be reachable from three subsystems and
# observable from outside. Both halves are tested: a mode nothing can reach is a
# code path, and a mode the host cannot observe cannot be tested at all — which
# M5 would discover when its escalation ladder had to tell degraded from
# restarted.
#
# The three entries are genuinely different mechanisms, not three callers of one
# function: one goes through a reset, two do not, and one needs a broken clock
# because no debugger write can make mtime disagree with itself.
test-safe: $(TARGET)
	@echo "safe: reachable from three subsystems, observable from outside"
	@$(MAKE) --no-print-directory safe-one WHICH=trap
	@$(MAKE) --no-print-directory safe-one WHICH=overrun
	@$(MAKE) --no-print-directory safe-clock
	@echo "PASS (three entry points, three distinct reasons)"

# $(WHICH) = trap | overrun
safe-one: $(TARGET)
	@printf "  %-9s " "$(WHICH)"
	@rm -f $(BUILD)/serial.log $(BUILD)/safe.log && touch $(BUILD)/serial.log
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    -serial file:$(BUILD)/serial.log -kernel $(TARGET) -s -S & \
	 qpid=$$!; sleep 1; \
	 if [ "$(WHICH)" = trap ]; then \
	   timeout 40 $(GDB) $(TARGET) -batch -ex 'set $$fault_mode = 1' \
	     -x harness/faults/fault.gdb > $(BUILD)/safe.log 2>&1; \
	   marker='FAULT-INJECTED'; want='trap on a previous boot'; \
	 else \
	   timeout 60 $(GDB) $(TARGET) -batch \
	     -x harness/faults/safemode.gdb > $(BUILD)/safe.log 2>&1; \
	   marker='SAFE-INJECTED'; want='frame overrun'; \
	 fi; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 40 ))); do \
	   grep -qE 'boot   : (ok|FAULT)' $(BUILD)/serial.log 2>/dev/null && break; \
	   kill -0 $$qpid 2>/dev/null || break; sleep 0.05; \
	 done; \
	 kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	 grep -qF "$$marker" $(BUILD)/safe.log \
	   || { echo "FAIL: nothing was injected, so this proved nothing"; \
	        tail -5 $(BUILD)/safe.log; exit 1; }; \
	 grep -qF "mode   : SAFE, $$want" $(BUILD)/serial.log \
	   || { echo "FAIL: safe mode was not entered, or not for the right reason"; \
	        grep -E 'mode|sched|boot' $(BUILD)/serial.log; exit 1; }; \
	 echo "ok - $$(grep -F 'mode   : SAFE' $(BUILD)/serial.log | head -1 | sed 's/mode   : //')"

# The clock entry needs a broken timer: no debugger write can make mtime
# disagree with itself across the three reads inside obc_mtime_read.
safe-clock:
	@printf "  %-9s " "clock"
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory MTIME_SRC=harness/broken/mtime_unstable.c \
	    $(TARGET) >/dev/null
	@rm -f $(BUILD)/serial.log && touch $(BUILD)/serial.log
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    -serial file:$(BUILD)/serial.log -kernel $(TARGET) & \
	 qpid=$$!; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 40 ))); do \
	   grep -qE 'boot   : (ok|FAULT)' $(BUILD)/serial.log 2>/dev/null && break; \
	   kill -0 $$qpid 2>/dev/null || break; sleep 0.05; \
	 done; \
	 kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	 grep -qF 'mode   : SAFE, clock would not settle' $(BUILD)/serial.log \
	   || { echo "FAIL: an unsettled clock did not degrade the system"; \
	        grep -E 'mode|sched|boot' $(BUILD)/serial.log; exit 1; }; \
	 echo "ok - SAFE, clock would not settle"
	@$(MAKE) --no-print-directory clean >/dev/null

# --- Host code lint -----------------------------------------------------------
#
# The host code rules require harness/ to be ruff clean. A check that silently
# skips when the tool is missing is worse than no check: the first machine
# without ruff would wave unverified code through while printing green.
#
# **A missing verification tool is a failure, not an abstention.** Same principle
# as deps-check, and the same reason: a discipline nobody can observe is not a
# property.
RUFF := .venv/bin/ruff

lint:
	@test -x $(RUFF) || { \
	  echo "FAIL: $(RUFF) is missing, so harness/ cannot be shown clean."; \
	  echo "This is a failure and not a skip: a check that quietly does nothing"; \
	  echo "would let the first machine without the tool publish green results."; \
	  echo; \
	  echo "  python3 -m venv .venv && .venv/bin/pip install ruff"; \
	  exit 1; }
	@$(RUFF) check harness/
	@$(RUFF) format --check harness/
	@echo "lint: harness/ is clean"

# --- Runtime dependency guard -------------------------------------------------
#
# The compiler emits calls the source never mentions: 64-bit division and shift
# become __udivdi3 and friends, a large struct initialiser becomes memcpy, a
# large zeroing becomes memset. None of these appear in a grep of flight/.
#
# Today a freestanding link fails on them, loudly, which is the desired
# behaviour. The risk this guard covers is the *fix*: adding -lgcc or -lc to
# make a link error go away pulls the helper in silently and the build goes
# green with a dependency nobody decided to take. That is the failure this
# catches, and the linker cannot.
#
# Two checks. The undefined set must be empty — a fully linked freestanding
# image resolves everything. And no helper may appear anywhere in the symbol
# table, which is what catches a library that was linked in deliberately.
FORBIDDEN_SYMS := __udivdi3|__umoddi3|__divdi3|__moddi3|__muldi3 \
                  |__ashldi3|__ashrdi3|__lshrdi3|__clzsi2|__ctzsi2 \
                  |__divsi3|__udivsi3|__modsi3|__umodsi3 \
                  |memcpy|memset|memmove|memcmp|strlen|strcpy \
                  |malloc|calloc|realloc|free|printf|sprintf|puts

# Undefined symbols that are allowed. Empty on purpose: adding a name here is a
# decision, and it should look like one in the diff.
ALLOWED_UNDEF :=

deps-check: $(TARGET)
	@undef=$$($(CROSS)nm --undefined-only $(TARGET) 2>/dev/null \
	          | awk '{print $$2}' | grep -v '^$$' | sort -u); \
	 if [ -n "$(ALLOWED_UNDEF)" ]; then \
	   undef=$$(echo "$$undef" | grep -vE '^($(ALLOWED_UNDEF))$$' || true); \
	 fi; \
	 if [ -n "$$undef" ]; then \
	   echo "FAIL: unresolved symbols in a freestanding image:"; \
	   echo "$$undef" | sed 's/^/  /'; exit 1; \
	 fi
	@found=$$($(CROSS)nm $(TARGET) 2>/dev/null | awk '{print $$NF}' \
	          | grep -xE '$(FORBIDDEN_SYMS)' | sort -u || true); \
	 if [ -n "$$found" ]; then \
	   echo "FAIL: compiler or library helpers linked into the flight image:"; \
	   echo "$$found" | sed 's/^/  /'; \
	   echo; \
	   echo "These are never written in flight/ — the compiler synthesised them,"; \
	   echo "or a library was linked to silence a link error. Neither is allowed:"; \
	   echo "the image must contain only code this project wrote or chose."; \
	   exit 1; \
	 fi
	@echo "deps: no compiler or library helpers linked in"

# --- Trap handler and fault record --------------------------------------------
#
# Three scenarios, in increasing order of what they rule out. Faults are made by
# moving the program counter to an unmapped address rather than by planting a
# trap instruction: flash is read-only, and a build carrying a deliberate fault
# would not be the binary under test.
TRAP_MODES := 1 2 3 4

test-trap: $(TARGET)
	@echo "trap: fault injection through the gdbstub"
	@for m in $(TRAP_MODES); do \
	   printf "  mode %s " $$m; \
	   $(MAKE) --no-print-directory FAULT_MODE=$$m trap-one || exit 1; \
	 done
	@echo "PASS"

trap-one: $(TARGET)
	@rm -f $(BUILD)/serial.log $(BUILD)/fault.log && touch $(BUILD)/serial.log
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    -serial file:$(BUILD)/serial.log -kernel $(TARGET) -s -S & \
	 qpid=$$!; sleep 1; \
	 timeout 30 $(GDB) $(TARGET) -batch -ex 'set $$fault_mode = $(FAULT_MODE)' \
	   -x harness/faults/fault.gdb > $(BUILD)/fault.log 2>&1; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 20 ))); do \
	   [ "$$(grep -c '=== OBC-Zero ===' $(BUILD)/serial.log 2>/dev/null)" -ge 2 ] && break; \
	   kill -0 $$qpid 2>/dev/null || break; sleep 0.05; \
	 done; \
	 kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	 grep -qF 'FAULT-INJECTED' $(BUILD)/fault.log \
	   || { echo "FAIL: no fault was injected, so this proved nothing"; \
	        cat $(BUILD)/fault.log; exit 1; }; \
	 banners=$$(grep -c '=== OBC-Zero ===' $(BUILD)/serial.log); \
	 test "$$banners" -eq 2 \
	   || { echo "FAIL: $$banners banners, expected exactly 2 (a loop or a hang)"; exit 1; }; \
	 line=$$(grep 'reset  :' $(BUILD)/serial.log | tail -1); \
	 case "$(FAULT_MODE)" in \
	   1|2) echo "$$line" | grep -qF 'trap mcause=0x00000001' \
	          || { echo "FAIL: expected the original cause, got:$$line"; exit 1; };; \
	   3)   echo "$$line" | grep -qF 'DOUBLE FAULT' \
	          || { echo "FAIL: expected a double fault, got:$$line"; exit 1; };; \
	   4)   echo "$$line" | grep -qF 'requested' \
	          || { echo "FAIL: a deliberate reset was not reported as one:$$line"; exit 1; }; \
	        echo "$$line" | grep -qF 'trap' \
	          && { echo "FAIL: a deliberate reset was read as a fault:$$line"; exit 1; }; \
	        true;; \
	 esac; \
	 echo "ok -$${line#*reset  :}"

# The validator must reject on either field alone. One test does not cover the
# other: checking only the magic passes the wrong-checksum case, and vice versa.
RECORD_MODES := 1 2 3

test-record: $(TARGET)
	@echo "record: magic and checksum rejected independently"
	@for m in $(RECORD_MODES); do \
	   printf "  mode %s " $$m; \
	   $(MAKE) --no-print-directory RECORD_MODE=$$m record-one || exit 1; \
	 done
	@echo "PASS"

record-one: $(TARGET)
	@rm -f $(BUILD)/serial.log $(BUILD)/rec.log && touch $(BUILD)/serial.log
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    -serial file:$(BUILD)/serial.log -kernel $(TARGET) -s -S & \
	 qpid=$$!; sleep 1; \
	 timeout 30 $(GDB) $(TARGET) -batch -ex 'set $$record_mode = $(RECORD_MODE)' \
	   -x harness/faults/record.gdb > $(BUILD)/rec.log 2>&1; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 20 ))); do \
	   grep -q 'boot   :' $(BUILD)/serial.log 2>/dev/null && break; \
	   kill -0 $$qpid 2>/dev/null || break; sleep 0.05; \
	 done; \
	 kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	 grep -qF 'RECORD-PLANTED' $(BUILD)/rec.log \
	   || { echo "FAIL: no record was planted, so this proved nothing"; \
	        cat $(BUILD)/rec.log; exit 1; }; \
	 line=$$(grep 'reset  :' $(BUILD)/serial.log | tail -1); \
	 case "$(RECORD_MODE)" in \
	   1) echo "$$line" | grep -qF 'RECORD CORRUPT' \
	        || { echo "FAIL: a wrong checksum was accepted:$$line"; exit 1; };; \
	   2) echo "$$line" | grep -qF 'none recorded' \
	        || { echo "FAIL: a wrong magic was accepted:$$line"; exit 1; };; \
	   3) echo "$$line" | grep -qF 'interrupt 11' \
	        || { echo "FAIL: the interrupt bit was lost:$$line"; exit 1; };; \
	 esac; \
	 echo "ok -$${line#*reset  :}"

# --- Poisoned-RAM boot --------------------------------------------------------
#
# QEMU zeroes guest RAM at cold boot; real hardware does not. Any structure that
# is meant to survive a reset is therefore tested against a friendlier machine
# than the one it will fly on, and a missing magic value or checksum passes.
#
# This target fills RAM with a seeded pattern through the gdbstub before
# releasing the CPU, so the image boots on dirty memory. Deterministic given
# POISON_SEED, which is reported so a failing run can be reproduced from it.
POISON_SEED ?= 1

$(BUILD)/poison.bin:
	@mkdir -p $(dir $@)
	@python3 -c "import random,sys; r=random.Random($(POISON_SEED)); \
	  sys.stdout.buffer.write(bytes(r.getrandbits(8) for _ in range(16384)))" > $@

test-poisoned: $(TARGET) $(BUILD)/poison.bin
	@echo "smoke: boot banner on poisoned RAM, seed=$(POISON_SEED)"
	@rm -f $(BUILD)/serial.log && touch $(BUILD)/serial.log
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    -serial file:$(BUILD)/serial.log -kernel $(TARGET) -s -S & \
	 qpid=$$!; \
	 sleep 1; \
	 $(GDB) $(TARGET) -batch -x harness/faults/poison.gdb > $(BUILD)/poison.log 2>&1; \
	 found=0; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 20 ))); do \
	   if grep -qF "$(SENTINEL)" $(BUILD)/serial.log 2>/dev/null; then found=1; break; fi; \
	   kill -0 $$qpid 2>/dev/null || break; \
	   sleep 0.05; \
	 done; \
	 kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	 cat $(BUILD)/serial.log; \
	 test $$found -eq 1 || { echo "FAIL: sentinel not seen, seed=$(POISON_SEED)"; \
	                         cat $(BUILD)/poison.log; exit 1; }; \
	 echo "PASS (seed=$(POISON_SEED))"

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
	@$(MAKE) --no-print-directory size-check
	@$(MAKE) --no-print-directory deps-check
	@$(MAKE) --no-print-directory lint
	@echo
	@$(MAKE) --no-print-directory test

# Halted at reset with a gdbstub on :1234. Start this first, then attach from
# another shell with `make attach`. Connecting before QEMU is listening reports
# a connection timeout rather than a refusal, which reads like a GDB fault and
# is not one.
gdb: $(TARGET)
	$(QEMU) -machine $(MACHINE) $(ICOUNT) -nographic -kernel $(TARGET) -s -S

# Attach to a `make gdb` already running in another shell.
attach: $(TARGET)
	$(GDB) $(TARGET) $(GDBFLAGS)

clean:
	rm -rf $(BUILD)

-include $(DEP)
