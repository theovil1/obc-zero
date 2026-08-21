# Force a frame overrun by moving the clock past the frame deadline.
#
# Entry point 2 of 3 into safe mode. The executive finds the deadline already
# behind it at the slack check, counts a frame overrun, and degrades.
#
# The clock is jumped rather than the tasks being made slow. A slow task would
# also overrun, but it would consume real guest time and the amount would depend
# on the emulated CPU speed; moving the clock is exact and costs nothing.
#
# The anchor is obc_mtime_read with an ignore count rather than a breakpoint on
# the dispatch loop. `dispatch` is static and -Os inlines it out of existence,
# which is the same trap that caught the first trace dump. obc_mtime_read is a
# real global symbol, and under -icount the number of calls is deterministic, so
# the count lands in the same place on every run.
#
# 3000 calls puts execution inside the first frame's idle wait, after its four
# dispatches. That the overrun lands on frame 0 does not weaken anything: the
# host discriminates on the *reason*, not the frame, so a frame-overrun entry is
# never confused with a mode restored from a previous boot.
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

set confirm off
set pagination off

set architecture riscv:rv32
target remote localhost:1234

break obc_sched_run
continue
delete

break obc_mtime_read
ignore 2 3000
continue
delete

set $lo = *(unsigned int *)0x0200BFF8
set {unsigned int}0x0200BFF8 = $lo + 4096
printf "SAFE-INJECTED frame overrun, clock moved from %u to %u\n", $lo, $lo + 4096

detach
quit
