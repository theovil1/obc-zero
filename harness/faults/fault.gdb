# Inject a fault and observe what the trap handler records.
#
# The faults are produced by moving the program counter to an unmapped address
# rather than by planting a trap instruction in the image: flash is read-only
# here, and a build variant carrying a deliberate fault would be a different
# binary from the one under test.
#
# $fault_mode selects the scenario:
#   1  fault with a valid sp        -> the cause must be recorded
#   2  fault with sp outside RAM    -> the ORIGINAL cause must be recorded,
#                                      not the handler's own store fault
#   3  fault inside the handler     -> double fault, distinct cause, no loop
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

set confirm off
set pagination off
maint set target-async off

set architecture riscv:rv32
target remote localhost:1234

# Let the first banner complete so the two boots are distinguishable in the log.
break print_tick_check
continue
delete

if $fault_mode == 3
  # Enter the handler once, stop after the re-entry flag is set, then fault
  # again from inside it. Anything earlier would be a first entry.
  break obc_trap_flag_set
  set $pc = 0
  continue
  delete
  printf "FAULT-INJECTED mode=3 re-entry from inside the handler\n"
  set $pc = 0
else
  if $fault_mode == 2
    # sp outside RAM. The handler must not care: it never touches it.
    set $sp = 0x40000000
    printf "FAULT-INJECTED mode=2 sp=%#x then pc=0\n", $sp
  else
    printf "FAULT-INJECTED mode=1 pc=0 with valid sp=%#x\n", $sp
  end
  set $pc = 0
end

detach
quit
