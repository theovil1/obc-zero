/*
 * OBC-Zero entry point.
 *
 * M0 scope: bring up the UART, print a boot banner identifying the build, and
 * park. No timers, no traps, no tasks. Everything the banner reports is a fact
 * the system can establish about itself at this milestone, and nothing more.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include "core/status.h"
#include "hal/uart.h"

/* Injected by the build. See BUILD_HASH in the Makefile. */
#ifndef OBC_BUILD_HASH
#define OBC_BUILD_HASH "unknown"
#endif

extern uint32_t __data_start[];
extern uint32_t __bss_end[];
extern uint32_t __stack_bottom[];
extern uint32_t __stack_top[];

/* Must match the fill written by the boot code in flight/boot/start.S. */
#define STACK_PAINT 0xDEADBEEFu

void obc_main(void);

/*
 * Deepest point the stack has reached so far, in bytes, found by walking the
 * paint pattern up from __stack_bottom. The first word still holding the paint
 * marks the low-water address, so everything above it has been touched.
 *
 * Bounded by the stack extent, which the linker fixes at build time.
 */
static uint32_t stack_high_water(void)
{
    const uint32_t *p = (const uint32_t *)__stack_bottom;

    while (p < (const uint32_t *)__stack_top && *p == STACK_PAINT) {
        p++;
    }

    return (uint32_t)((uintptr_t)__stack_top - (uintptr_t)p);
}

static obc_status_t print_stack_usage(void)
{
    uint32_t reserved = (uint32_t)((uintptr_t)__stack_top - (uintptr_t)__stack_bottom);
    obc_status_t st;

    st = obc_uart_puts("stack  : ");
    if (st != OBC_OK) {
        return st;
    }
    st = obc_uart_put_u32(stack_high_water());
    if (st != OBC_OK) {
        return st;
    }
    st = obc_uart_puts(" B peak of ");
    if (st != OBC_OK) {
        return st;
    }
    st = obc_uart_put_u32(reserved);
    if (st != OBC_OK) {
        return st;
    }
    return obc_uart_puts(" B reserved\r\n");
}

/*
 * Reports how much of the 16 KiB of RAM the image consumes, measured from the
 * live linker symbols rather than from the build log. On a board this small,
 * this number matters more than any other on the banner.
 */
static obc_status_t print_ram_usage(void)
{
    uint32_t used = (uint32_t)((uintptr_t)__stack_top - (uintptr_t)__data_start);
    obc_status_t st;

    st = obc_uart_puts("ram    : ");
    if (st != OBC_OK) {
        return st;
    }
    st = obc_uart_put_u32(used);
    if (st != OBC_OK) {
        return st;
    }
    return obc_uart_puts(" B of 16384 B\r\n");
}

void obc_main(void)
{
    obc_status_t st;

    obc_uart_init();

    /*
     * The banner is the only observable this milestone has. If a write fails
     * there is nowhere left to report it: the UART *is* the reporting channel.
     * The status is still checked rather than discarded, so that the failure
     * path exists and is visible when M1 gives it somewhere to go.
     */
    st = obc_uart_puts("\r\n=== OBC-Zero ===\r\n");
    if (st == OBC_OK) {
        st = obc_uart_puts("build  : " OBC_BUILD_HASH "\r\n");
    }
    if (st == OBC_OK) {
        st = obc_uart_puts("board  : sifive_e\r\n");
    }
    if (st == OBC_OK) {
        st = obc_uart_puts("entry  : ");
    }
    if (st == OBC_OK) {
        st = obc_uart_put_hex32((uint32_t)(uintptr_t)&obc_main);
    }
    if (st == OBC_OK) {
        st = obc_uart_puts("\r\n");
    }
    if (st == OBC_OK) {
        st = print_ram_usage();
    }
    if (st == OBC_OK) {
        st = print_stack_usage();
    }

    /*
     * Sentinel. This exact string is what the host watches for to decide that
     * the run has reached its end state; seeing it, the harness stops QEMU
     * rather than waiting out a timeout. Changing it breaks `make test`.
     */
    if (st == OBC_OK) {
        (void)obc_uart_puts("boot   : ok\r\n");
    }

    /*
     * Park. M1 replaces this with the scheduler idle path; until then a halted
     * core is the honest end state, and `wfi` keeps the host CPU quiet during
     * long harness runs.
     */
    for (;;) {
        __asm__ volatile("wfi");
    }
}
