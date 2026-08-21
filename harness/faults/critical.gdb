# Corrupt copies of the critical state and observe what the voter does.
#
# $critical_copies : how many copies to corrupt, 1 or 2
# $critical_first  : which copy to corrupt first, 0=a 1=b 2=c
# $critical_bit    : which bit to flip in the value word
#
# One copy per run, and the copy is a parameter rather than a constant, because
# a recovery path tested by a single injection proves that injection site and
# not the path. The three copies are not symmetric in the code — one is read
# first, one is compared against, one settles the vote — so a voter exercised
# only against the first proves the first comparison works and nothing more.
# M3 already produced that exact failure with safe mode's clock entry.
#
# The corruption flips a bit in the value and leaves the checksum alone, which
# is what a single-event upset in the stored word looks like. Corrupting the
# checksum instead is a different fault and gets its own case.
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

set confirm off
set pagination off

set architecture riscv:rv32

# The port is a parameter, not a constant. Every injector used to assume it
# owned 1234, which meant two runs could not coexist: the second QEMU failed to
# bind, the debugger attached to the first one's target, and the failures named
# subsystems that were working perfectly. Defaults to 1234 for the single-run
# targets that predate this.
if $_isvoid($campaign_port)
  set $campaign_port = 1234
end
eval "target remote localhost:%d", $campaign_port

# Before the window opens, so the first vote of the first frame is the one that
# meets the corruption.
break obc_sched_run
continue
delete

set $copies = (unsigned int *[3]){ \
  (unsigned int *)&obc_critical_a, \
  (unsigned int *)&obc_critical_b, \
  (unsigned int *)&obc_critical_c }

set $i = 0
while $i < $critical_copies
  set $which = ($critical_first + $i) % 3
  set $p = $copies[$which]
  set $before = *$p
  set *$p = $before ^ (1 << $critical_bit)
  printf "CRITICAL-INJECTED copy=%d value %u -> %u (bit %d)\n", \
    $which, $before, *$p, $critical_bit
  set $i = $i + 1
end

detach
quit
