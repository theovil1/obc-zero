# Read the execution trace and the scheduler state out of guest RAM.
#
# No breakpoint and no continue. The firmware parks after its window closes, so
# attaching to the already-running target halts it in a state where nothing
# further writes the trace. That removes the whole class of fragility that comes
# with breaking on a function the optimiser is free to inline — which is exactly
# what happened to the first version of this script: `print_sched_summary` is
# static, -Os inlined it, and the breakpoint resolved to an address the flow
# never reached in the way GDB expected.
#
# The task table is dumped alongside the results on purpose. The checker derives
# what it expects from the binary under test rather than from constants restated
# on the host: a duplicated expectation drifts, and when it does the assertion
# starts checking the host's opinion instead of the flight software's table.
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

set confirm off
set pagination off

set architecture riscv:rv32
target remote localhost:1234

printf "window_frames %u\n", 16
printf "frame_ticks %u\n", 1024
printf "task_count %u\n", obc_task_count
printf "frames_run %u\n", obc_frames_run
printf "trace_len %u\n", obc_trace_len
printf "trace_overflow %u\n", obc_trace_overflow
printf "frame_overruns %u\n", obc_frame_overruns
printf "slack_min %u\n", obc_slack_ticks_min
printf "mode %u\n", obc_mode
printf "safe_reason %u\n", obc_safe_reason
printf "safe_entry_frame %u\n", obc_safe_entry_frame
printf "window_start %u\n", obc_window_start_ticks
printf "window_end %u\n", obc_window_end_ticks

set $i = 0
while $i < obc_task_count
  printf "task %u %s %u %u %u %u %u %u\n", $i, \
    obc_task_table[$i].name, \
    obc_task_table[$i].period_frames, \
    obc_task_table[$i].budget_instr, \
    obc_task_table[$i].essential, \
    obc_task_state[$i].runs, \
    obc_task_state[$i].overruns, \
    obc_task_state[$i].max_instr
  set $i = $i + 1
end

set $i = 0
while $i < obc_trace_len
  printf "trace %u %u\n", $i, obc_trace[$i]
  set $i = $i + 1
end

printf "DUMP-COMPLETE\n"

detach
quit
