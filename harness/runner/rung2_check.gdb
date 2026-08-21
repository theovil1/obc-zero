# Rung 2 must be reachable in the image that flies.
#
# ADR 0007 left the middle rung empty rather than hollow: an absent rung is
# visible in the ladder, a hollow one is visible only in a campaign, late. M6
# fills it with the reset_fn each task declares, and this is what stops it from
# quietly emptying again — drop telemetry's reset_fn and the ladder returns to
# two steps wearing three labels, with nothing to say so.
#
# Read from the binary, on the host, and not by flight code. A vehicle cannot
# grow a subsystem in response to finding it has none, so a runtime check could
# only degrade a working system over a build-configuration mistake. The first
# version did exactly that, and took two unrelated scheduler tests with it.
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
  if obc_task_table[$i].reset_fn != 0
    printf "  %s owns a subsystem\n", obc_task_table[$i].name
    set $n = $n + 1
  end
  set $i = $i + 1
end
printf "RUNG2-REACHABLE %d of %d tasks\n", $n, obc_task_count
quit
