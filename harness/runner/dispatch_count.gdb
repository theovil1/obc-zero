# How many dispatches the task table produces over the assertion window.
#
# Derived from the table in the binary rather than typed into the test. The
# literal was 30, and adding one period-1 task at M7 made it 46 — the assertion
# then failed naming the voter, which had nothing to do with it. A number a test
# holds about the system under test has to come from the system under test.
#
# No target: this reads the ELF's own initialised data.
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

set confirm off
set pagination off
set architecture riscv:rv32

set $i = 0
set $n = 0
while $i < obc_task_count
  set $n = $n + (obc_sched_window_frames / obc_task_table[$i].period_frames)
  set $i = $i + 1
end
printf "DISPATCHES %d\n", $n
quit
