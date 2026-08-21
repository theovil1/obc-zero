/*
 * Link script for the QEMU `sifive_e` machine.
 *
 * Every address here is taken from the machine model as instantiated by QEMU,
 * recorded in emu/sifive_e-mtree.txt and justified in docs/adr/0001-target-platform.md.
 * Do not edit an address in this file without updating both.
 *
 * Execution model: the mask ROM at 0x1000 jumps unconditionally to 0x20400000,
 * inside the XIP flash region. Code and read-only data therefore execute in
 * place from flash, while .data must be copied down into RAM at boot and .bss
 * zeroed. RAM is 16 KiB total and holds .data, .bss and the stack.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

OUTPUT_ARCH("riscv")
OUTPUT_FORMAT("elf32-littleriscv")
ENTRY(_start)

MEMORY
{
    /*
     * The QEMU xip region spans 0x20000000-0x3fffffff (512 MiB), but that is an
     * emulator artefact: the HiFive1 this machine models carries 16 MiB of SPI
     * flash. We cap FLASH at 4 MiB so that an image which links here also fits
     * a real FE310 board, rather than designing against the emulator.
     */
    FLASH (rx)  : ORIGIN = 0x20400000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x80000000, LENGTH = 16K
}

/*
 * Stack size. Sized from measurement, not from habit: the M0 boot path peaks
 * at 64 bytes, read back from the paint pattern at run time. 1 KiB is sixteen
 * times that, which covers the trap frame M1 adds and the task call depth M2
 * adds, and returns 3 KiB to the allocation in docs/BUDGET.md.
 *
 * Two things keep this honest: the ASSERT below fails the build on overflow,
 * and the banner reports the live high-water mark on every boot. Revisit at M2
 * with a measurement taken under scheduler load, not at the banner.
 */
__stack_size = 1K;

SECTIONS
{
    .init : ALIGN(4)
    {
        KEEP(*(.init))
    } > FLASH

    .text : ALIGN(4)
    {
        /*
         * Deliberate-reset entry point. It has no caller in flight code yet —
         * M5 supplies the policy that decides when to ask for a reset — so
         * --gc-sections drops it, and the harness test that exercises it then
         * fails to find the symbol.
         *
         * Kept rather than given an artificial caller: the mechanism belongs to
         * M1 and the decision to use it belongs to M5. Inventing a call site to
         * satisfy the linker would misrepresent both.
         */
        KEEP(*(.text.obc_fault_reset_with_cause))
        *(.text .text.*)
        . = ALIGN(4);
    } > FLASH

    .rodata : ALIGN(4)
    {
        *(.srodata .srodata.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > FLASH

    /*
     * .data lives in RAM but is stored in flash. __data_lma is where the
     * startup code copies from; __data_start/__data_end bound the destination.
     */
    .data : ALIGN(4)
    {
        __data_start = .;
        *(.sdata .sdata.*)
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT> FLASH

    __data_lma = LOADADDR(.data);

    /* gp-relative addressing window, centred on the small-data area. */
    PROVIDE(__global_pointer$ = __data_start + 0x800);

    /*
     * The three copies of critical state, deliberately not adjacent.
     *
     * Interleaved with .bss and .noinit rather than padded apart: the
     * separation is what matters, and buying it with padding would spend RAM on
     * a gap. A corruption walking forward from one copy meets several hundred
     * bytes of unrelated data before reaching the next, and a burst wide enough
     * to span two would have to cross .bss to do it.
     *
     * See flight/core/critical.c and docs/adr/0006-what-is-critical.md.
     */
    .critical0 (NOLOAD) : ALIGN(4)
    {
        KEEP(*(.critical0))
    } > RAM

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    /*
     * Survives a warm reset: never zeroed, never copied into. The startup code
     * skips this range deliberately, which is what makes the reset cause
     * readable on the next boot.
     *
     * Placed between .bss and the stack rather than at the top of RAM: a stack
     * overflow grows down into the guard region below __stack_bottom, not into
     * the fault record, so the record survives the very failure it exists to
     * report.
     */
    .critical1 (NOLOAD) : ALIGN(4)
    {
        KEEP(*(.critical1))
    } > RAM

    .noinit (NOLOAD) : ALIGN(4)
    {
        __noinit_start = .;
        KEEP(*(.noinit))
        KEEP(*(.noinit.*))
        . = ALIGN(4);
        __noinit_end = .;
    } > RAM

    /*
     * Guard. Interleaving with .bss and .noinit gives 404 bytes between the
     * first two copies and only 56 between the last two, because .noinit is
     * small — and 56 bytes is fourteen words, which a wild pointer crosses
     * without noticing.
     *
     * This buys the missing separation explicitly. It is the one place in the
     * system where RAM holding nothing is doing something: the distance is the
     * mechanism. Charged to M4's line in docs/BUDGET.md, which was allocated
     * for exactly this and is otherwise barely touched.
     */
    .critical_guard (NOLOAD) : ALIGN(4)
    {
        __critical_guard_start = .;
        . += 320;
        __critical_guard_end = .;
    } > RAM

    .critical2 (NOLOAD) : ALIGN(4)
    {
        KEEP(*(.critical2))
    } > RAM

    .stack (NOLOAD) : ALIGN(16)
    {
        __stack_bottom = .;
        . += __stack_size;
        . = ALIGN(16);
        __stack_top = .;
    } > RAM

    /*
     * Hard failure at link time rather than a mysterious crash at run time.
     * There is no heap and no MMU: overflowing 16 KiB is not recoverable.
     */
    ASSERT(__stack_top <= ORIGIN(RAM) + LENGTH(RAM),
           "RAM overflow: .data + .bss + stack exceed 16 KiB")

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
    }
}
