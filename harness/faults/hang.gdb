# Hang the executive itself, and check the hardware backstop resets the machine.
#
# Not a task. A hung task is caught by the budget check, which runs inside the
# very executive a hung executive would take with it — so a watchdog test that
# hangs a task proves the watchdog works in the cases where everything else
# already does. The path that matters is the one that runs when the rest has
# failed, which is the system-level shape of the M1 mscratch defect.
#
# So this stops the executive dead, mid-window, by parking the program counter
# in the idle loop that obc_main enters only after the window closes. Nothing
# feeds the watchdog from there. If the AON backstop is doing its job the
# machine resets on its own; if it is not, the run hangs until the host gives up
# and that is a failure.
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

set confirm off
set pagination off
maint set target-async off

set architecture riscv:rv32
target remote localhost:1234

# A few frames in, so the hang lands on a running executive rather than on one
# that has not started.
break obc_sched_run
continue
delete

break obc_mtime_read
ignore 2 3000
continue
delete

# The park loop at the end of obc_main: a real infinite loop in real flight
# code, reached in a way the flow never would.
set $park = &obc_park_forever
set $pc = $park
printf "HANG-INJECTED executive parked at %#x, nothing feeds the watchdog now\n", $pc

detach
quit
