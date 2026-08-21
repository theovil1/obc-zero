# Fill RAM with a seeded pseudo-random pattern before releasing the CPU.
#
# QEMU zeroes guest RAM when the process starts. Real hardware powers up with
# whatever the cells happen to hold. Any test of a structure that survives a
# reset — the reset cause above all — is therefore flattered by the emulator: an
# unprotected read returns a clean zero here and noise on silicon.
#
# This script removes the flattery. The image under test boots on dirty RAM, so
# a missing magic value or a missing checksum fails here rather than on the
# first real board.
#
# Driven by `make test-poisoned`. The pattern is generated from a seed on the
# host, so a failing run is reproducible from the seed alone.
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

set confirm off
set pagination off

# Must precede the connection: on an already-attached target GDB auto-detects
# and falls back to its host architecture.
set architecture riscv:rv32
target remote localhost:1234

# The whole of DTIM RAM, 0x80000000-0x80003fff. The startup code will repaint
# the stack and zero .bss over the top of this; whatever it does not touch keeps
# the poison, which is exactly the region a persistent structure lives in.
restore build/poison.bin binary 0x80000000

# Leave QEMU running. The host watches the serial stream for the sentinel.
detach
quit
