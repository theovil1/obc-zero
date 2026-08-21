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

# The telemetry sources, selectable one at a time so a broken variant replaces
# exactly the thing under test. A single TLM_SRC would mean swapping the sensor
# validator also swapped the frame packer, and a failure could not be attributed.
# UART_SRC selects the serial HAL. test-uart-stall swaps in one whose downlink
# reports itself full for a chosen number of polls, because QEMU's device cannot
# hold a stall for longer than a single retired instruction.
UART_SRC ?= flight/hal/uart.c

TLM_FRAME_SRC ?= flight/tlm/frame.c
TLM_SENSOR_SRC ?= flight/tlm/sensor.c
TLM_SRC := $(TLM_FRAME_SRC) $(TLM_SENSOR_SRC)

# TASKS_SRC selects the task table. test-sched-overrun swaps in one whose budget
# is far below what its task costs.
TASKS_SRC ?= flight/core/tasks.c

# The two serial ports, in the order QEMU assigns them: the first -serial is
# UART0 and the second is UART1. Named here rather than repeated, because a
# target that forgets the second one gets a downlink writing into nothing and a
# capture that is empty rather than wrong — which reads as "no frames" and not
# as "misconfigured run".
CONSOLE_LOG = $(BUILD)/serial.log
DOWNLINK_BIN = $(BUILD)/tlm.bin
PORTS = -serial file:$(CONSOLE_LOG) -serial file:$(DOWNLINK_BIN)

SRC_C := flight/core/main.c \
         flight/core/critical.c \
         flight/core/mode.c \
         flight/core/recover.c \
         $(SCHED_SRC) \
         $(TASKS_SRC) \
         flight/core/fault.c \
         $(MTIME_SRC) \
         $(TLM_SRC) \
         $(UART_SRC)
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

.PHONY: all build run deps-check lint guard-check test-wdt test-loop test-voter voter-one test-voter-campaign test-safe safe-one safe-clock test test-sched test-sched-repro test-sched-broken sched-expect-reject test-sched-overrun sched-expect-overrun test-trap trap-one test-record record-one test-stability test-poisoned test-carry carry-one test-carry-broken test-carry-expect-fault measure gdb attach size size-check size-accept clean

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

# --- Survival guard ---------------------------------------------------------
#
# **Every automated run asserts that the system survived it.**
#
# This exists because of a run that reset the machine twice while its test
# reported PASS. The test exercised a degradation path — a refused downlink — and
# asserted everything about the path: the frame was shed, the drop was counted,
# the announcement went out. It never asserted that the vehicle was still there
# afterwards, and two reset banners sat in the log with nothing looking at them.
#
# That is this repository's own bidirectional rule turning on it. The effect was
# observed; the *absence of unwanted effects* was not. And it is the one defect
# here that nothing would have caught except by accident, because every
# individual assertion was correct.
#
# So the count is not something each test remembers to check. It is a declaration
# every run must make, and a run that boots more often than its test declared is
# a failure whatever else passed.
#
# $(1) = fewest boots allowed, $(2) = most, $(3) = the console log
#
# A range rather than a number because two tests legitimately loop: a hardware
# watchdog reset and a reset-loop escalation do not produce a fixed count. A
# range is still a declaration — it says what the test expects to survive — and
# a range that spans everything is the same as no guard, so it is written
# narrowly and the reason is given at the call site.
define assert_boots
	@boots=$$(grep -c '=== OBC-Zero ===' $(3) 2>/dev/null || true); \
	 boots=$${boots:-0}; \
	 if [ "$$boots" -lt "$(1)" ] || [ "$$boots" -gt "$(2)" ]; then \
	   echo ""; \
	   echo "FAIL: the run booted $$boots times; this test declares $(1)..$(2)."; \
	   echo "A reset the test did not declare is a failure whatever else passed:"; \
	   echo "the path under test may be correct and the system still gone."; \
	   grep -nE '=== OBC-Zero ===|recover: rung|reset  :' $(3) 2>/dev/null \
	     | sed 's/^/    /' | head -12; \
	   exit 1; \
	 fi
endef

# The guard is only a default if it cannot be forgotten.
#
# Every automated run — every $(QEMU) launched with -display none — must be
# followed by a declaration. Counted here rather than trusted, on the same
# principle as deps-check: a control nobody can skip is worth more than one
# everybody remembers.
#
# `make run` and `make gdb` are exempt and are the only two: they are
# interactive, they write no console log, and a person is watching.
#
# Both counts are anchored on recipe lines — a leading tab — because the first
# version counted the patterns inside its own recipe and reported 17 runs against
# 15. A control that matches its own text is a control measuring itself.
guard-check:
	@runs=$$(awk '/^\t.*\$$\(QEMU\).* -display none/ {n++} END{print n+0}' Makefile); \
	 guards=$$(awk '/^\t\$$\(call assert_boots,/ {n++} END{print n+0}' Makefile); \
	 if [ "$$runs" != "$$guards" ]; then \
	   echo "FAIL: $$runs automated runs, $$guards survival declarations."; \
	   echo "A run with no declaration cannot fail when the system does not"; \
	   echo "survive it, which is how a test came to report PASS across two"; \
	   echo "machine resets. Add a \$$(call assert_boots,MIN,MAX,LOG) after it."; \
	   exit 1; \
	 fi; \
	 echo "guard: $$runs automated runs, each declaring what it expects to survive"

# --- Pinned reference ---------------------------------------------------------
#
# docs/BUDGET.md is a register of claims about memory consumption. A claim is
# only worth something if a figure cannot move without someone noticing, so the
# per-section sizes are pinned in a versioned reference and compared on every
# measurement. Any drift fails the build.
#
# The count of deliberate status discards is pinned the same way, and for the
# same reason. OBC_IGNORE is better than the bare (void) cast most projects use
# because each abandonment has to be written as one — but a number nobody
# watches is not a control. It went from 9 to 13 during a single milestone
# without anyone deciding that it should, which is the whole argument: unpinned,
# the mechanism documents the drift instead of containing it.
#
# Raising it is allowed and sometimes right. Raising it silently is not.
#
# The point is not to freeze the image. It is to make every size change a
# deliberate act: run `make size-accept` and say why in the commit message.
#
# Debug sections are excluded on purpose. They are not loaded, they do not
# consume flash or RAM, and they move with compiler flags that have nothing to
# do with the budget.
SIZE_SECTIONS := \.init|\.text|\.rodata|\.data|\.bss|\.stack|\.critical[0-2]
SIZE_REF      := docs/size-reference.txt
SIZE_ACTUAL   := $(BUILD)/size-actual.txt

# The reference carries a "# accepted <date> <reason>" line that the generated
# file cannot have, so the comparison strips it. The reason is there to be read
# by a person asking why a figure is what it is, not to be diffed.

# The toolchain version is part of the reference. A compiler upgrade changes
# code size for reasons that have nothing to do with this project, and without
# this header the result is a drift failure with no explanation in it. With it,
# the diff says "gcc moved" on its first line.
$(SIZE_ACTUAL): $(TARGET)
	@mkdir -p $(dir $@)
	@{ echo "# gcc      $$($(CC) -dumpversion)"; \
	   echo "# binutils $$($(CROSS)ld --version | head -1 | grep -oE '[0-9]+\.[0-9]+[.0-9]*' | head -1)"; \
	   echo "# ignores  $$(grep -rho 'OBC_IGNORE(' flight/ --include='*.c' | wc -l | tr -d ' ')"; \
	   $(SIZE) -A $(TARGET) \
	     | grep -E '^($(SIZE_SECTIONS))[[:space:]]' \
	     | awk '{ printf "%-10s %6d\n", $$1, $$2 }' \
	     | LC_ALL=C sort; } > $@

size-check: $(SIZE_ACTUAL)
	@test -f $(SIZE_REF) || { \
	  echo "no size reference at $(SIZE_REF)."; \
	  echo "Create it with 'make size-accept' and commit it."; exit 1; }
	@grep -v '^# accepted ' $(SIZE_REF) > $(BUILD)/size-ref-figures.txt
	@if diff -u $(BUILD)/size-ref-figures.txt $(SIZE_ACTUAL) > $(BUILD)/size.diff 2>&1; then \
	  echo "size: matches $(SIZE_REF)"; \
	else \
	  echo "FAIL: a pinned figure drifted from $(SIZE_REF)"; \
	  echo; sed '1,2d' $(BUILD)/size.diff; echo; \
	  echo "If this change is intended, run 'make size-accept' and explain it"; \
	  echo "in the commit message. If it is not, you have just found something."; \
	  echo "An 'ignores' line that moved means the system stopped caring about a"; \
	  echo "return value somewhere new. That is a decision, so decide it."; \
	  exit 1; \
	fi

# Accepting a new reference requires a reason, and the reason is written into
# the reference itself.
#
# The earlier version printed "say why in the commit message" and trusted that
# to happen. It did not: the discard count went from 13 to 14 one milestone
# after the rule was written, and the commit said nothing. A guard that detects
# drift is half a control; one that refuses to move without an explanation is
# the whole of it. Same shape as deps-check, which does not ask nicely whether
# libgcc was linked in.
size-accept: $(SIZE_ACTUAL)
	@test -n "$(REASON)" || { \
	  echo "REFUSED: no reason given."; \
	  echo; \
	  echo "This file is a register of claims. Moving a figure without saying"; \
	  echo "why leaves the next reader with a number and no way to ask about it,"; \
	  echo "and that is exactly how a count drifts from 13 to 14 unremarked."; \
	  echo; \
	  echo "  make size-accept REASON='the scrubber discards a status it cannot use'"; \
	  exit 1; }
	@{ head -n 3 $(SIZE_ACTUAL); \
	   echo "# accepted $$(date +%Y-%m-%d) $(REASON)"; \
	   tail -n +4 $(SIZE_ACTUAL); } > $(SIZE_REF)
	@echo "size reference updated:"
	@cat $(SIZE_REF)

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
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none $(PORTS) \
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
	# a smoke boot resets for no reason at all
	$(call assert_boots,1,1,$(BUILD)/serial.log)

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
	    $(PORTS) -kernel $(TARGET) -s -S & \
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
	# a forced carry must not cost a boot
	$(call assert_boots,1,1,$(BUILD)/serial.log)
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
	    $(PORTS) -kernel $(TARGET) -s & \
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
	# dumping the trace must not have rebooted the run it describes
	$(call assert_boots,1,1,$(BUILD)/serial.log)
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

# --- Escalation ---------------------------------------------------------------
#
# The watchdog test breaks the *executive*, not a task. A hung task is caught by
# the budget check, which runs inside the very executive a hung executive takes
# with it — so hanging a task proves the watchdog works in the cases where
# everything else already does.
# **Waits for the line it is about to read, not for the banner above it.**
# Waiting on the second banner and then killing QEMU is a race: the boot counter
# is printed several lines later, and about one run in eight was killed in
# between. The test then reported "a hardware reset was not counted as a short
# boot" — a red naming the mechanism under test, caused by the harness. A false
# red is the expensive kind: it teaches people not to believe failures.
test-wdt: $(TARGET)
	@echo "wdt: a hung executive is reset by the hardware backstop"
	@rm -f $(BUILD)/serial.log $(BUILD)/hang.log && touch $(BUILD)/serial.log
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    $(PORTS) -kernel $(TARGET) -s -S & \
	 qpid=$$!; sleep 1; \
	 timeout 60 $(GDB) $(TARGET) -batch -x harness/faults/hang.gdb \
	   > $(BUILD)/hang.log 2>&1; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 40 ))); do \
	   [ "$$(grep -c 'boots  :' $(BUILD)/serial.log 2>/dev/null)" -ge 2 ] \
	     && break; \
	   kill -0 $$qpid 2>/dev/null || break; sleep 0.05; \
	 done; \
	 kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	 grep -qF 'HANG-INJECTED' $(BUILD)/hang.log \
	   || { echo "FAIL: the executive was not hung, so this proved nothing"; \
	        tail -4 $(BUILD)/hang.log; exit 1; }; \
	 n=$$(grep -c '=== OBC-Zero ===' $(BUILD)/serial.log); \
	 test "$$n" -ge 2 \
	   || { echo "FAIL: a hung executive was not reset ($$n banners)"; exit 1; }; \
	 grep -qF 'boots  : 1 consecutive short' $(BUILD)/serial.log \
	   || { echo "FAIL: a hardware reset was not counted as a short boot"; \
	        grep boots $(BUILD)/serial.log; exit 1; }
	# the hardware backstop resets a hung executive; not a fixed count, because the reset lands wherever the hang left the frame
	$(call assert_boots,2,6,$(BUILD)/serial.log)
	@echo "PASS (reset, and the hardware reset was counted)"

# A fault that recurs every boot must climb to the top rung and stop there.
# Asserted on the streak reaching its limit AND on safe mode being entered
# instead of a sixth reset — the second is what stops the ladder cycling.
test-loop:
	@echo "loop: a recurring fault reaches safe mode instead of resetting forever"
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory TASKS_SRC=harness/broken/tasks_loop.c \
	    $(TARGET) >/dev/null
	@rm -f $(BUILD)/serial.log && touch $(BUILD)/serial.log
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    $(PORTS) -kernel $(TARGET) & \
	 qpid=$$!; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 60 ))); do \
	   grep -qF 'mode   : SAFE' $(BUILD)/serial.log 2>/dev/null && break; \
	   kill -0 $$qpid 2>/dev/null || break; sleep 0.05; \
	 done; \
	 kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	 grep -qF 'boots  : 5 consecutive short' $(BUILD)/serial.log \
	   || { echo "FAIL: the streak never reached its limit"; \
	        grep boots $(BUILD)/serial.log | tail -3; exit 1; }; \
	 grep -qF 'mode   : SAFE, reset loop' $(BUILD)/serial.log \
	   || { echo "FAIL: the ladder kept resetting instead of degrading"; exit 1; }; \
	 after=$$(sed -n '/mode   : SAFE, reset loop/,$$p' $(BUILD)/serial.log \
	          | grep -c 'recover: rung 3'); \
	 test "$$after" -eq 0 \
	   || { echo "FAIL: it reset $$after more times after reaching safe mode"; exit 1; }
	# a reset loop, deliberately, until the streak reaches its limit and safe mode stops it
	$(call assert_boots,2,12,$(BUILD)/serial.log)
	@echo "PASS (streak reached the limit, degraded, and stopped resetting)"
	@$(MAKE) --no-print-directory clean >/dev/null

# --- Voter --------------------------------------------------------------------
#
# Once per copy, never once on whichever copy is convenient. The three are not
# symmetric in the code — one is read first, one is compared against, one
# settles the vote — and a recovery path tested by a single injection proves
# that injection site rather than the path. M3 produced exactly that failure
# with safe mode's clock entry, which was wired to one of three call sites.
CRIT_COPIES := 0 1 2

test-voter: $(TARGET)
	@echo "voter: single-copy corruption, once per copy"
	@for c in $(CRIT_COPIES); do \
	   printf "  copy %s " $$c; \
	   $(MAKE) --no-print-directory CRIT_N=1 CRIT_FIRST=$$c CRIT_BIT=$$c voter-one \
	     || exit 1; \
	 done
	@echo "  double-copy corruption, once per pair"
	@for c in $(CRIT_COPIES); do \
	   printf "  pair %s " $$c; \
	   $(MAKE) --no-print-directory CRIT_N=2 CRIT_FIRST=$$c CRIT_BIT=5 voter-one \
	     || exit 1; \
	 done
	@echo "PASS"

# $(CRIT_N) copies corrupted starting at $(CRIT_FIRST), flipping bit $(CRIT_BIT).
voter-one: $(TARGET)
	@rm -f $(BUILD)/serial.log $(BUILD)/crit.log && touch $(BUILD)/serial.log
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    $(PORTS) -kernel $(TARGET) -s -S & \
	 qpid=$$!; sleep 1; \
	 timeout 40 $(GDB) $(TARGET) -batch \
	   -ex 'set $$critical_copies = $(CRIT_N)' \
	   -ex 'set $$critical_first = $(CRIT_FIRST)' \
	   -ex 'set $$critical_bit = $(CRIT_BIT)' \
	   -x harness/faults/critical.gdb > $(BUILD)/crit.log 2>&1; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 40 ))); do \
	   grep -qE 'boot   : (ok|FAULT)' $(BUILD)/serial.log 2>/dev/null && break; \
	   kill -0 $$qpid 2>/dev/null || break; sleep 0.05; \
	 done; \
	 timeout 20 $(GDB) $(TARGET) -batch -ex 'set confirm off' \
	   -ex 'set architecture riscv:rv32' -ex 'target remote localhost:1234' \
	   -ex 'printf "repairs=%u failed_votes=%u mode=%u\n", obc_critical_repairs, obc_critical_failed_votes, obc_mode' \
	   > $(BUILD)/crit-state.txt 2>&1; \
	 kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	 grep -qF 'CRITICAL-INJECTED' $(BUILD)/crit.log \
	   || { echo "FAIL: nothing was corrupted, so this proved nothing"; \
	        tail -5 $(BUILD)/crit.log; exit 1; }; \
	 state=$$(grep -o 'repairs=[0-9]* failed_votes=[0-9]* mode=[0-9]*' $(BUILD)/crit-state.txt); \
	 if [ "$(CRIT_N)" = 1 ]; then \
	   echo "$$state" | grep -qE 'repairs=[1-9]' \
	     || { echo "FAIL: a single corrupted copy was not repaired ($$state)"; exit 1; }; \
	   echo "$$state" | grep -qF 'failed_votes=0' \
	     || { echo "FAIL: a single corruption should still leave a majority ($$state)"; exit 1; }; \
	   echo "$$state" | grep -qF 'mode=0' \
	     || { echo "FAIL: a repaired corruption changed behaviour ($$state)"; exit 1; }; \
	   grep -qF '30 dispatches' $(BUILD)/serial.log \
	     || { echo "FAIL: a repaired corruption changed the dispatch count"; exit 1; }; \
	 else \
	   echo "$$state" | grep -qE 'failed_votes=[1-9]' \
	     || { echo "FAIL: two corrupted copies still produced a verdict ($$state)"; exit 1; }; \
	   echo "$$state" | grep -qF 'mode=1' \
	     || { echo "FAIL: an unresolvable vote did not fail safe ($$state)"; exit 1; }; \
	 fi; \
	 echo "ok - $$state"
	# a repaired or an unresolvable vote degrades; neither resets
	$(call assert_boots,1,1,$(BUILD)/serial.log)

# Randomised corruption campaign. Deterministic given CAMPAIGN_SEED, which is
# printed on every run and recorded in the report: a campaign whose seed is not
# written down cannot be re-run, and a result that cannot be re-run is an
# anecdote.
#
# The full acceptance criterion is 1000 runs. Each takes about two seconds of
# host time, so 1000 is roughly half an hour and is run deliberately rather than
# as part of the ordinary suite. CAMPAIGN_RUNS defaults to a sample that proves
# the machinery.
CAMPAIGN_SEED ?= 1
CAMPAIGN_RUNS ?= 20

# Live view while it runs. A campaign takes long enough that the only
# alternative to watching it is not watching it, and a campaign nobody watches
# is one where a systematic failure is found at the end instead of at run ten.
#
# Allocates a port per run, so it no longer blocks every other test for half an
# hour. Writes a dated report naming its seed: a result that cannot be replayed
# is an anecdote.
test-voter-campaign: $(TARGET)
	@$(PY) harness/runner/campaign.py \
	   --runs $(CAMPAIGN_RUNS) --seed $(CAMPAIGN_SEED) --elf $(TARGET)

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
	@$(MAKE) --no-print-directory safe-one WHICH=trap SAFE_BOOTS=2
	@$(MAKE) --no-print-directory safe-one WHICH=overrun SAFE_BOOTS=1
	@$(MAKE) --no-print-directory safe-clock
	@echo "PASS (three entry points, three distinct reasons)"

# $(WHICH) = trap | overrun, $(SAFE_BOOTS) = how many boots that entry costs
#
# The two entries do not cost the same and the declaration says so. A trap resets
# and comes up degraded on the *next* boot, because the handler resets rather than
# running policy; an overrun degrades in place. A single number covering both
# would be a range wide enough to hide the difference, and the difference is the
# thing ADR 0005 is about.
SAFE_BOOTS ?= 1

safe-one: $(TARGET)
	@printf "  %-9s " "$(WHICH)"
	@rm -f $(BUILD)/serial.log $(BUILD)/safe.log && touch $(BUILD)/serial.log
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    $(PORTS) -kernel $(TARGET) -s -S & \
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
	# trap resets and degrades on the next boot; overrun degrades in place
	$(call assert_boots,$(SAFE_BOOTS),$(SAFE_BOOTS),$(BUILD)/serial.log)

# The clock entry needs a broken timer: no debugger write can make mtime
# disagree with itself across the three reads inside obc_mtime_read.
safe-clock:
	@printf "  %-9s " "clock"
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory MTIME_SRC=harness/broken/mtime_unstable.c \
	    $(TARGET) >/dev/null
	@rm -f $(BUILD)/serial.log && touch $(BUILD)/serial.log
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    $(PORTS) -kernel $(TARGET) & \
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
	# a clock that will not settle degrades in place
	$(call assert_boots,1,1,$(BUILD)/serial.log)
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
	    $(PORTS) -kernel $(TARGET) -s -S & \
	 qpid=$$!; sleep 1; \
	 timeout 30 $(GDB) $(TARGET) -batch -ex 'set $$fault_mode = $(FAULT_MODE)' \
	   -x harness/faults/fault.gdb > $(BUILD)/fault.log 2>&1; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 20 ))); do \
	   [ "$$(grep -c 'reset  :' $(BUILD)/serial.log 2>/dev/null)" -ge 2 ] && break; \
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
	# the trap resets exactly once, and the second boot is the one that reports it
	$(call assert_boots,2,2,$(BUILD)/serial.log)

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
	    $(PORTS) -kernel $(TARGET) -s -S & \
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
	# a rejected record is read on the boot after the one that wrote it
	$(call assert_boots,1,2,$(BUILD)/serial.log)

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
	    $(PORTS) -kernel $(TARGET) -s -S & \
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
	# poisoned RAM must be survived, not rebooted through
	$(call assert_boots,1,1,$(BUILD)/serial.log)

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
	@echo "basis   : emulated (icount shift=6) - instruction and timing figures"
	@echo "          below do not transfer to hardware, see docs/EMULATION-GAP.md"
	@echo
	@$(SIZE) $(TARGET)
	@echo
	@$(MAKE) --no-print-directory size-check
	@$(MAKE) --no-print-directory deps-check
	@$(MAKE) --no-print-directory lint
	@$(MAKE) --no-print-directory guard-check
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

# --- M6: telemetry -------------------------------------------------------- #

# Nominal. No injection: the frames must decode against the layout read from the
# binary, and nothing rejected may appear as a value.
#
# Note what this does *not* prove. With no harness attached the mock backend
# holds whatever survived reset, so both sensors are correctly reported as
# untrustworthy — which exercises "flagged, not propagated" and says nothing
# about "unflagged means real". That direction needs an injected good sensor,
# which is what test-tlm-sensors does.
test-tlm: $(TARGET)
	@rm -f $(CONSOLE_LOG) $(DOWNLINK_BIN)
	@timeout $(RUN_TIMEOUT_S) $(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    $(PORTS) -kernel $(TARGET) >/dev/null 2>&1 || true
	# nominal telemetry costs no boot
	$(call assert_boots,1,1,$(CONSOLE_LOG))
	@grep -qF 'tlm    : layout ok' $(CONSOLE_LOG) \
	  || { echo "FAIL: the descriptor audit did not pass, so no frame is trustworthy"; \
	       exit 1; }
	@python3 harness/runner/tlm_check.py --elf $(TARGET) --capture $(DOWNLINK_BIN)

# One injected sensor case. $(SENSOR_IDX) is the sensor, $(SENSOR_MODE) is which
# of the three failures from ADR 0008 to produce, $(SENSOR_EXPECT) is the
# property the host must then be able to demonstrate.
tlm-one: $(TARGET)
	@rm -f $(CONSOLE_LOG) $(DOWNLINK_BIN) $(BUILD)/tlm-inject.log
	@touch $(CONSOLE_LOG) $(DOWNLINK_BIN)
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    $(PORTS) -kernel $(TARGET) -s -S & \
	 qpid=$$!; sleep 1; \
	 timeout 60 $(GDB) $(TARGET) -batch \
	   -ex 'set $$sensor_index = $(SENSOR_IDX)' \
	   -ex 'set $$sensor_mode = $(SENSOR_MODE)' \
	   -x harness/faults/sensor.gdb > $(BUILD)/tlm-inject.log 2>&1; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 40 ))); do \
	   grep -qE 'boot   : (ok|FAULT)' $(CONSOLE_LOG) 2>/dev/null && break; \
	   kill -0 $$qpid 2>/dev/null || break; sleep 0.05; \
	 done; \
	 kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	 grep -qF 'SENSOR-INJECT' $(BUILD)/tlm-inject.log \
	   || { echo "FAIL: nothing was injected, so this proved nothing"; \
	        tail -5 $(BUILD)/tlm-inject.log; exit 1; }; \
	 python3 harness/runner/tlm_check.py --elf $(TARGET) --capture $(DOWNLINK_BIN) \
	   --expect $(SENSOR_EXPECT) --sensor $(SENSOR_IDX)
	# a lying sensor is flagged, never escalated to a reset
	$(call assert_boots,1,1,$(CONSOLE_LOG))

# Every detectable failure, on every sensor.
#
# Both sensors, because a validator exercised on one proves that one: the
# descriptor is indexed, and an index used once is an index that has never been
# shown to be applied per sensor. M3 produced exactly that failure with safe
# mode's clock entry, on one of three call sites.
test-tlm-sensors: $(TARGET)
	@echo "sensor cases, per sensor and per failure mode"
	@for s in 0 1; do \
	   for c in "0 honest" "1 range" "2 stuck"; do \
	     set -- $$c; \
	     printf "  sensor %s %-7s " $$s $$2; \
	     $(MAKE) --no-print-directory SENSOR_IDX=$$s SENSOR_MODE=$$1 \
	       SENSOR_EXPECT=$$2 tlm-one || exit 1; \
	   done; \
	 done
	@echo "PASS"

# --- M6: the detectors, shown failing ------------------------------------- #
#
# A test that has never failed has not been shown to work. Each of these builds
# an image with one detector deliberately removed and requires the corresponding
# check to REFUSE. A green here would mean the check passes on a broken build,
# which is worse than no check at all.

# A validator that rejects nothing. The range and stuck cases must both fail.
#
# `clean` before every variant, and it is not caution. Switching TLM_SENSOR_SRC
# changes the object list without changing any timestamp, so make finds
# build/obc.elf newer than its new prerequisites and skips the link entirely —
# and the case then runs against the previous image. The first version of this
# target did exactly that and reported PASS while testing the flight validator
# against itself: a green that certified nothing, which is the failure this whole
# section exists to prevent.
test-tlm-blind-broken:
	@echo "sensor validator with both detectors removed"
	@for c in "1 range" "2 stuck"; do \
	   set -- $$c; \
	   printf "  %-6s " $$2; \
	   $(MAKE) --no-print-directory clean >/dev/null; \
	   if $(MAKE) --no-print-directory TLM_SENSOR_SRC=harness/broken/sensor_blind.c \
	        SENSOR_IDX=0 SENSOR_MODE=$$1 SENSOR_EXPECT=$$2 tlm-one >/dev/null 2>&1; then \
	     echo "FAIL: a validator that rejects nothing passed the $$2 check"; exit 1; \
	   fi; \
	   echo "refused, as it must"; \
	 done
	@$(MAKE) --no-print-directory clean >/dev/null
	@echo "PASS"

# A validator that rejects everything. Only the second direction can catch it,
# and this is the target that proves the second direction is doing work.
test-tlm-paranoid-broken:
	@echo "sensor validator that rejects every reading"
	@$(MAKE) --no-print-directory clean >/dev/null
	@printf "  honest "
	@if $(MAKE) --no-print-directory TLM_SENSOR_SRC=harness/broken/sensor_paranoid.c \
	      SENSOR_IDX=0 SENSOR_MODE=0 SENSOR_EXPECT=honest tlm-one >/dev/null 2>&1; then \
	   echo "FAIL: a validator that flags good readings passed the honest check,"; \
	   echo "      so only the 'bad is flagged' direction is being tested"; exit 1; \
	 fi
	@echo "refused, as it must"
	@$(MAKE) --no-print-directory clean >/dev/null
	@echo "PASS"

# A descriptor table citing a wrong offset. The audit must refuse before a single
# frame is emitted, and the announcement must come from the flight software.
test-tlm-layout-broken:
	@echo "descriptor table with one wrong offset"
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory TLM_FRAME_SRC=harness/broken/tlm_layout_wrong.c \
	   build >/dev/null 2>&1
	@rm -f $(BUILD)/tlm-broken.bin
	@timeout $(RUN_TIMEOUT_S) $(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    -serial file:$(BUILD)/console-broken.log -serial file:$(BUILD)/tlm-broken.bin -kernel $(TARGET) >/dev/null 2>&1 || true
	# a failed layout audit degrades and keeps running
	$(call assert_boots,1,1,$(BUILD)/console-broken.log)
	@grep -qF 'LAYOUT AUDIT FAILED' $(BUILD)/console-broken.log \
	  || { echo "FAIL: a table with a wrong offset passed its own audit"; \
	       $(MAKE) --no-print-directory clean >/dev/null; exit 1; }
	@if python3 harness/runner/tlm_check.py --elf $(TARGET) \
	     --capture $(BUILD)/tlm-broken.bin >/dev/null 2>&1; then \
	   echo "FAIL: frames were emitted against a layout that failed its audit"; \
	   $(MAKE) --no-print-directory clean >/dev/null; exit 1; \
	 fi
	@echo "  refused before emitting, and said so on the serial line"
	@$(MAKE) --no-print-directory clean >/dev/null
	@echo "PASS"

# Rung 2 must be reachable in the image that flies.
#
# ADR 0007 left the middle rung empty rather than hollow, on the grounds that an
# absent rung is visible in the ladder and a hollow one is visible only in a
# campaign. M6 fills it, and this is what stops it from quietly emptying again:
# drop telemetry's reset_fn and the ladder silently returns to two steps wearing
# three labels.
#
# Checked from the binary, on the host, and not by flight code. The vehicle
# cannot grow a subsystem in response, so a runtime check would only be able to
# degrade a system over a build-configuration mistake — which the first version
# of this did, taking two unrelated scheduler tests with it.
test-rung2-reachable: $(TARGET)
	@echo "ladder: rung 2 must be reachable in the flight table"
	@$(GDB) $(TARGET) -batch -nx -x harness/runner/rung2_check.gdb \
	   > $(BUILD)/rung2.txt 2>&1
	@sed -n 's/^  /    /p' $(BUILD)/rung2.txt
	@grep -qE 'RUNG2-REACHABLE [1-9]' $(BUILD)/rung2.txt \
	  || { echo "FAIL: no task declares a reset_fn, so rung 2 is unreachable and"; \
	       echo "      the ladder is two steps wearing three labels"; \
	       cat $(BUILD)/rung2.txt; exit 1; }
	@echo "PASS"

test-tlm-all: test-tlm test-tlm-sensors test-rung2-reachable test-tlm-blind-broken \
              test-tlm-paranoid-broken test-tlm-layout-broken

# --- The downlink refuses -------------------------------------------------- #
#
# The reservation M6 shipped with, turned into a test. It was recorded as a
# backlog item, and recording it was the wrong response: a green result on a path
# the emulator cannot exercise does not say the path is fine, it says the path
# was not tested.
#
# Two runs of one image, with the downlink refusing and not, and the assertion is
# the difference between them:
#
#     stalled - nominal  >=  allowance x poll cost      and   over == 0
#
# A bracket, from both sides, and neither side is a tolerance. The floor proves
# the retries are spent rather than declared — a build giving up on the first
# refusal shows no difference at all. The ceiling is the executive's own overrun
# count, which is the budget the task is judged against, applied by the thing that
# judges it and decides whether the ladder gets climbed.
#
# A lower bound and not an equality, because the two paths differ in more than
# their polling: the refused one announces the drop and skips forty-five byte
# writes, and predicting the net of those would be predicting the compiler.
#
# A *difference* rather than an absolute figure, because the substitute adds
# instructions outside the poll loop that the flight build does not pay. Taking
# both runs through the same image cancels them.
#
# **And the run must boot exactly once.** The first version of this target did
# not check that, and the run it passed had climbed the ladder to rung 3 and reset
# the machine over a congested downlink — the one outcome ADR 0009 exists to
# prevent, happening while the test reported PASS.
uart-stall-build:
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory UART_SRC=harness/broken/uart_stalled.c \
	   build >/dev/null 2>&1
	@grep -qF 'uart_stalled' $(BUILD)/obc.map \
	  || { echo "FAIL: the stalling UART is not in the image, so every case below"; \
	       echo "      would run against the flight port and prove nothing"; exit 1; }

# One run of the already-built stub. $(STALL_ON) says whether the port refuses.
uart-stall-one:
	@rm -f $(CONSOLE_LOG) $(DOWNLINK_BIN) $(BUILD)/stall.log
	@touch $(CONSOLE_LOG) $(DOWNLINK_BIN)
	@$(QEMU) -machine $(MACHINE) $(ICOUNT) -display none \
	    $(PORTS) -kernel $(TARGET) -s -S & \
	 qpid=$$!; sleep 1; \
	 timeout 60 $(GDB) $(TARGET) -batch -nx \
	   -ex 'set $$stall_on = $(STALL_ON)' \
	   -x harness/faults/uart_stall.gdb > $(BUILD)/stall.log 2>&1; \
	 for i in $$(seq 1 $$(( $(RUN_TIMEOUT_S) * 40 ))); do \
	   grep -qE 'boot   : (ok|FAULT)' $(CONSOLE_LOG) 2>/dev/null && break; \
	   kill -0 $$qpid 2>/dev/null || break; sleep 0.05; \
	 done; \
	 kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null
	# the whole point: a refused downlink must not cost the machine
	$(call assert_boots,1,1,$(CONSOLE_LOG))
	@grep -qF 'UART-STALL-SET' $(BUILD)/stall.log \
	  || { echo "FAIL: the injector never ran, so this proved nothing"; \
	       tail -5 $(BUILD)/stall.log; exit 1; }

# A downlink emitter that gives up on the first refusal instead of waiting.
#
# The target that makes test-uart-stall mean something. Every behavioural
# assertion passes against this build — shed, counted, announced, inside budget,
# one boot — because giving up instantly does all of those, cheaply. Only the
# cost floor catches it, and the frames it costs are frames that did not need to
# be lost.
#
# **The per-byte allowance, which is the defect ADR 0009 actually closed, is not
# testable this way and is not claimed to be.** With the port refusing from the
# first byte, a per-byte emitter abandons exactly as early as a per-frame one and
# costs the same; they diverge only on a port that refuses intermittently, which
# this machine cannot produce without instrumentation that costs more than the
# thing being measured. What contains it is the single allowance declared outside
# the byte loop — visible in the disassembly as one `li a4,256` hoisted above it —
# and the compile-time assertion in flight/core/tasks.c. Said here rather than
# left for someone to assume the suite covers it.
test-uart-impatient:
	@echo "an emitter that gives up instead of waiting"
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory UART_SRC=harness/broken/uart_impatient.c \
	   build >/dev/null 2>&1
	@grep -qF 'uart_impatient' $(BUILD)/obc.map \
	  || { echo "FAIL: the broken UART is not in the image"; exit 1; }
	@$(MAKE) --no-print-directory STALL_ON=0 uart-stall-one
	@$(PY) harness/runner/stall_check.py --elf $(TARGET) --capture $(DOWNLINK_BIN) \
	    --console $(CONSOLE_LOG) --record-cost $(BUILD)/stall-nominal.txt
	@$(MAKE) --no-print-directory STALL_ON=1 uart-stall-one
	@if $(PY) harness/runner/stall_check.py --elf $(TARGET) \
	     --capture $(DOWNLINK_BIN) --console $(CONSOLE_LOG) --one-boot \
	     --no-overrun --expect-drops --nominal-cost $(BUILD)/stall-nominal.txt \
	     > $(BUILD)/impatient.txt 2>&1; then \
	   echo "FAIL: an emitter that never waits passed every check, so the floor"; \
	   echo "      is not doing any work and the retries are unproven"; \
	   $(MAKE) --no-print-directory clean >/dev/null; exit 1; \
	 fi
	@grep -F 'STALL-CHECK FAIL' $(BUILD)/impatient.txt | sed 's/^/    /' | cut -c1-100
	@$(MAKE) --no-print-directory clean >/dev/null
	@echo "PASS (refused, as it must)"

test-uart-stall:
	@echo "downlink refusal: the frame is shed, never the system"
	@$(MAKE) --no-print-directory uart-stall-build
	@printf "  port accepting  "
	@$(MAKE) --no-print-directory STALL_ON=0 uart-stall-one
	@cp $(CONSOLE_LOG) $(BUILD)/stall-console-off.log
	@$(PY) harness/runner/stall_check.py --elf $(TARGET) \
	    --capture $(DOWNLINK_BIN) --console $(CONSOLE_LOG) --one-boot \
	    --record-cost $(BUILD)/stall-nominal.txt
	@printf "  port refusing   "
	@$(MAKE) --no-print-directory STALL_ON=1 uart-stall-one
	@$(PY) harness/runner/stall_check.py --elf $(TARGET) \
	    --capture $(DOWNLINK_BIN) --console $(CONSOLE_LOG) --one-boot \
	    --no-overrun --expect-drops --nominal-cost $(BUILD)/stall-nominal.txt
	@$(MAKE) --no-print-directory clean >/dev/null
	@echo "PASS"
