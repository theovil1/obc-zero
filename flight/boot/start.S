/*
 * Reset entry for OBC-Zero on QEMU `sifive_e`.
 *
 * The mask ROM at 0x1000 jumps here (0x20400000) with nothing initialised:
 * no stack, no gp, .data still in flash and .bss full of whatever the RAM
 * happens to contain. This file establishes a valid C environment and nothing
 * more.
 *
 * Every loop below is bounded by a linker-provided address range.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/fault.h"

    .section .init, "ax"
    .global _start
    .type _start, @function
_start:
    /* Relaxation must stay off until gp actually holds __global_pointer$,
     * otherwise the assembler may rewrite these loads as gp-relative. */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    la      sp, __stack_top

    /*
     * mscratch and mtvec are established here, before anything that can fault.
     *
     * A trap taken before mscratch is loaded would find it holding whatever the
     * reset value is — zero on this machine — and the handler would write the
     * fault record to address 0. A trap taken before mtvec is installed jumps
     * to mtvec's reset value, also zero. Both windows are closed by doing this
     * first, ahead of the .data copy and the .bss clear, either of which could
     * in principle fault on a corrupted image.
     *
     * The banner asserts that mscratch still holds this value, so the invariant
     * is checked at run time rather than assumed from the position of these
     * four instructions.
     */
    la      t0, obc_fault_record
    csrw    mscratch, t0

    /*
     * Clear the re-entry flag explicitly, and note why this rather than putting
     * it under the magic-and-checksum discipline that protects the cause.
     *
     * The flag answers "am I already inside the handler", which is a question
     * about the current boot only. Cleared here, it means exactly that. Left
     * persistent, it would instead mean "some previous boot died inside the
     * handler" — a different and also useful fact, but not the one the handler
     * needs at entry, and conflating them would make a double fault in one boot
     * indistinguishable from a stale flag in the next.
     *
     * The clear must happen here because at cold boot this word holds whatever
     * the RAM held. On real hardware that is noise, and a pattern that happens
     * to be non-zero would make the first trap of the first boot report a
     * double fault. QEMU zeroes RAM and would hide that entirely, which is why
     * the poisoned-RAM run exists.
     */
    sw      zero, OBC_FR_IN_HANDLER(t0)

    la      t0, obc_trap_entry
    csrw    mtvec, t0

    /* Mask every interrupt source. Exceptions still reach the handler. */
    csrw    mie, zero
    csrw    mip, zero

    /* Copy .data from its load address in flash to its run address in RAM.
     * Bounded by __data_start..__data_end. */
    la      t0, __data_lma
    la      t1, __data_start
    la      t2, __data_end
1:  bgeu    t1, t2, 2f
    lw      t3, 0(t0)
    sw      t3, 0(t1)
    addi    t0, t0, 4
    addi    t1, t1, 4
    j       1b
2:

    /* Zero .bss. Bounded by __bss_start..__bss_end. */
    la      t0, __bss_start
    la      t1, __bss_end
3:  bgeu    t0, t1, 4f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       3b
4:

    /*
     * Paint the stack. With only 16 KiB of RAM, knowing the true high-water
     * mark is worth four instructions; M2 reads this pattern back.
     * Bounded by __stack_bottom..__stack_top.
     */
    la      t0, __stack_bottom
    la      t1, __stack_top
    li      t2, 0xDEADBEEF
5:  bgeu    t0, t1, 6f
    sw      t2, 0(t0)
    addi    t0, t0, 4
    j       5b
6:

    call    obc_main

    /*
     * obc_main must not return. If it does, that is a defect, not a normal
     * exit: park the core rather than fall through into whatever follows.
     * M1 replaces this with a recorded reset cause.
     */
7:  wfi
    j       7b

    .size _start, . - _start
