# Plant a fault record directly in .noinit before the CPU is released, to test
# the validator's two rejections separately.
#
# One test does not cover the other: a validator that checks only the magic
# passes the wrong-checksum case, and one that checks only the checksum passes
# the wrong-magic case. Both are exercised, in the same spirit as the naive
# reader in the carry test.
#
# $record_mode:
#   1  valid magic, correct payload, WRONG checksum -> must report CORRUPT
#   2  correct checksum, WRONG magic               -> must report no record
#   3  valid magic and checksum                    -> must report the trap
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

set confirm off
set pagination off
maint set target-async off

set architecture riscv:rv32
target remote localhost:1234

# The startup code clears in_handler but never touches the rest of the record,
# so planting it here survives into obc_main.
break obc_main
continue
delete

set $r = (unsigned int *)&obc_fault_record
# payload: cause, epc, tval, reset_cause
set $r[2] = 0x8000000B
set $r[3] = 0x20400123
set $r[4] = 0xCAFEBABE
set $r[5] = 1
set $ck = 0x5A5AC3C3 ^ $r[2] ^ $r[3] ^ $r[4] ^ $r[5]

if $record_mode == 1
  set $r[1] = $ck ^ 0x1
  set $r[0] = 0x0BCFA017
  printf "RECORD-PLANTED mode=1 magic ok, checksum wrong\n"
end
if $record_mode == 2
  set $r[1] = $ck
  set $r[0] = 0x0BCFA018
  printf "RECORD-PLANTED mode=2 checksum ok, magic wrong\n"
end
if $record_mode == 3
  set $r[1] = $ck
  set $r[0] = 0x0BCFA017
  printf "RECORD-PLANTED mode=3 both correct\n"
end

detach
quit
