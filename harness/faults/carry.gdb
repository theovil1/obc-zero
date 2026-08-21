# Force one mtime low-word carry to land inside a 64-bit read.
#
# Waiting for a carry is not a test strategy: one happens every 2^32 ticks, once
# every 36 hours at 32768 Hz, and under -icount the run is deterministic, so it
# either meets the window or never will. Never would leave a permanently green
# test on a broken clock.
#
# One crossing per run, on purpose. Chaining several inside a single monitored
# sequence is not possible: after a crossing the counter sits just above the
# boundary, and bringing it back below for the next one is a forward jump of
# about 4.29e9 ticks, which the firmware's bounded-progression check correctly
# rejects as implausible. The detector is strong enough to constrain how its own
# test can be built. Repeated carry propagation is therefore covered by running
# this script over a series of high-word values, which also exercises bit
# patterns a run of consecutive crossings would never reach.
#
# Parameters, both set on the command line:
#   $carry_line  file:line of the second of the two register reads
#   $carry_hi    high word to cross from, i.e. the crossing is $carry_hi -> +1
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

set confirm off
set pagination off

set architecture riscv:rv32
target remote localhost:1234

# --- Stage 1: park the counter just below the chosen boundary -----------------
#
# Done at the entry of the self-check rather than at reset. Between reset and
# here the banner is printed, which takes many ticks; a value preset at reset
# would have carried naturally long before the check began.
break check_tick_counter
continue

set {unsigned int}0x0200BFFC = $carry_hi
set {unsigned int}0x0200BFF8 = 0xFFFFFF00
delete

# --- Stage 2: cross the boundary between the two reads ------------------------
#
# `break $carry_line` is rejected: GDB requires an integer for a line-spec
# convenience variable. eval formats the string in first.
eval "break %s", $carry_line

# The first read establishes the baseline the loop compares against, and it must
# be taken *before* the carry. Injecting into it would move the baseline along
# with everything else, leaving a delta of zero and a test that proves nothing —
# which is exactly what the first version of this script did.
continue
continue

set {unsigned int}0x0200BFF8 = 0x00000005
set {unsigned int}0x0200BFFC = $carry_hi + 1
delete

# Proof that the injection actually happened. Without it, a script that errors
# out early leaves the firmware running undisturbed and the test reports a pass
# on an injection that never occurred. This is the general rule for every
# injector: assert that the fault landed, never infer it from a return code.
printf "CARRY-INJECTED hi=%u->%u\n", $carry_hi, $carry_hi + 1

detach
quit
