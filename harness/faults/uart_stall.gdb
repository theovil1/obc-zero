# Hold the downlink refusing, and see what the emitter does about it.
#
# $stall_on     : 1 to refuse, 0 to leave the port alone
# $stall_emits  : how many emissions to refuse before releasing the port
#
# **It has to release.** Held refusing for the whole run, every frame is shed and
# none survives to carry the drop count out — which is a true property of a dead
# downlink and not the one under test here. The count is only readable from the
# ground on a link that comes back, so the injector breaks the link and mends it.
#
# The subject is not the UART. It is that a loop inside a budgeted flight task was
# bounded by a constant nobody derived from anything — 100000 polls, per byte, in
# a task budgeted 3000 instructions — and that no campaign had ever seen it matter
# because the emulated port always accepts. ADR 0009.
#
# **This drives a HAL substitute, not the device.** QEMU's sifive_uart models the
# FIFO and sets the full bit, but drains it on a bottom half that runs the moment
# the vCPU resumes: a bit set from here is clear again after one retired
# instruction, measured with `stepi`. So the refusal is applied where the flight
# code reads. The substitute picks its status source *outside* the poll loop, so
# the loop it runs is instruction for instruction the flight build's — which is
# what makes the cost measured through it mean anything.
#
# Two runs of the same image, with and without, and the difference between them
# is the assertion. A difference cancels whatever constant offset the substitute
# adds elsewhere; an absolute figure would not.
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

set confirm off
set pagination off

set architecture riscv:rv32

if $_isvoid($campaign_port)
  set $campaign_port = 1234
end
eval "target remote localhost:%d", $campaign_port

if $_isvoid($stall_on)
  set $stall_on = 1
end
if $_isvoid($stall_emits)
  set $stall_emits = 3
end

# Before the executive opens its window, so every dispatch in the run sees the
# same port. Setting it partway through would make the reported worst dispatch
# depend on which frame the debugger happened to reach.
break obc_sched_run
continue
delete

set obc_uart_stall_active = $stall_on
if $stall_on
  set obc_uart_stall_status = 0x80000000
else
  set obc_uart_stall_status = 0
end

printf "UART-STALL-SET on=%d status=0x%08x allowance=%u poll=%u\n", \
  obc_uart_stall_active, obc_uart_stall_status, \
  (unsigned int)obc_uart_tx_retry_total, (unsigned int)obc_uart_tx_poll_instr

# Let the refusal last a set number of emissions, then mend the link so the
# frames that follow can carry the count out.
if $stall_on
  break obc_uart_downlink_write
  set $n = 0
  while $n < $stall_emits
    continue
    set $n = $n + 1
  end
  delete
  set obc_uart_stall_active = 0
  set obc_uart_stall_status = 0
  printf "UART-STALL-RELEASED after %d emissions\n", $stall_emits
end

detach
quit
