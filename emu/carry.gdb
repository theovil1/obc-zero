# Force the mtime low-word carry to land inside a 64-bit read.
#
# The defect this provokes is a race between the two halves of a 64-bit read on
# RV32. Waiting for it is not an option: a carry happens every 2^32 ticks, once
# every 36 hours at 32768 Hz, and under -icount the system is deterministic, so
# a run either meets the window or never does. "Never" would leave a permanently
# green test on a broken clock — the same hollow criterion this milestone exists
# to remove.
#
# So the race is forced rather than awaited. The test is then independent of the
# timebase entirely.
#
# This is also the first exercise of the fault-injection primitives against a
# genuine defect rather than an intentionally broken build: breakpoint on a
# chosen instruction, write guest state while halted, resume.
#
# Requires $carry_line to be set on the command line, pointing at the second of
# the two register reads. See the CARRY-INJECT markers.
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

set confirm off
set pagination off

set architecture riscv:rv32
target remote localhost:1234

# --- Stage 1: park the counter just below a carry -----------------------------
#
# Done at the entry of the self-check rather than at reset. Between reset and
# here the banner is printed, which takes many ticks; a value preset at reset
# would have carried naturally long before the check began.
break check_tick_counter
continue

set $h = *(unsigned int *)0x0200BFFC
set {unsigned int}0x0200BFF8 = 0xFFFFFF00
set {unsigned int}0x0200BFFC = $h
delete

# --- Stage 2: cross the boundary between the two reads ------------------------
#
# The breakpoint sits on the second register read, so when it fires the first
# read has already been taken with the pre-carry value.
# `break $carry_line` is rejected: GDB requires an integer for a line spec
# convenience variable. eval formats the string in first.
eval "break %s", $carry_line

# The first read establishes the baseline the loop compares against, and it must
# be taken *before* the carry. Injecting into it would move the baseline along
# with everything else, leaving a delta of zero and a test that proves nothing —
# which is exactly what the first version of this script did.
#
# So let the baseline read through, and inject into the one after it.
continue
continue

set {unsigned int}0x0200BFF8 = 0x00000005
set {unsigned int}0x0200BFFC = $h + 1
delete

# Proof that the injection actually happened. Without it, a script that errors
# out early leaves the firmware running undisturbed and the test reports a pass
# on an injection that never occurred.
printf "CARRY-INJECTED hi=%u->%u\n", $h, $h + 1

# Let it run. A correct reader notices the high word moved and retries; a naive
# one returns a value about 2^32 away from the truth. The firmware's bounded
# progression check is what tells them apart, and it reports its verdict on the
# serial line either way.
detach
quit
