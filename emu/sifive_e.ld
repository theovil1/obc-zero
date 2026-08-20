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

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
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
