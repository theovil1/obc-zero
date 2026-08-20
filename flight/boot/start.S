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

    /* Mask every interrupt source. M0 has no trap vector installed yet, so a
     * trap taken here would jump to whatever mtvec happens to hold. */
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
