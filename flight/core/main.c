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

#include "core/fault.h"
#include "core/status.h"
#include "hal/mtime.h"
#include "hal/uart.h"

/*
 * Clock plausibility bound.
 *
 * Two reads taken back to back cannot legitimately be a second apart, so any
 * gap at or above one second of ticks means the clock is lying rather than
 * running fast. The bound is derived from the measured timebase, not from a
 * guessed constant, so a change of machine rescales it with everything else.
 *
 * This is deliberately not a monotonicity check. The dangerous naive read
 * (low word then high word) returns a value about 2^32 too large at a carry,
 * which is still strictly increasing: a monotonicity assertion passes on a
 * clock that is off by thirty-six hours. Bounded progression does not.
 */
#define TICK_DELTA_MAX ((uint64_t)OBC_MTIME_HZ)

/*
 * Reads taken during the boot self-check.
 *
 * Overridable at build time so that two distinct properties can be tested
 * separately rather than conflated:
 *
 *   - repeated carry propagation, which needs few reads because the carries are
 *     forced into them one at a time through the debugger;
 *   - read stability, which needs many reads and no debugger at all.
 *
 * The default is the small one. `make test-stability` builds the large one.
 */
#ifndef TICK_CHECK_READS
#define TICK_CHECK_READS 64u
#endif

/* Injected by the build. See BUILD_HASH in the Makefile. */
#ifndef OBC_BUILD_HASH
#define OBC_BUILD_HASH "unknown"
#endif

/*
 * The build hash is held in its own section rather than concatenated into a
 * string literal, so that it stays out of the mergeable string pool.
 *
 * Inside that pool the linker tail-merges strings that are suffixes of others,
 * and whether a given merge succeeds turns out to depend on the *content* of
 * the strings present. That made the image size vary by 4 bytes between two
 * commits with byte-identical source, purely because their hashes differed,
 * which quietly destroys footprint tracking across commits. Measured and
 * recorded in docs/LOGBOOK.md.
 */
static const char build_hash[] __attribute__((section(".rodata.buildid"), used)) =
    OBC_BUILD_HASH;

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

/*
 * Boot clock self-check. Takes TICK_CHECK_READS successive readings and
 * verifies that the counter advances without ever moving backwards and without
 * ever jumping further than TICK_DELTA_MAX.
 *
 * Reports the largest gap observed, so a passing run still says how much margin
 * it had rather than only that it passed.
 */
static obc_status_t check_tick_counter(uint64_t *max_delta_out, uint64_t *span_out)
{
    uint64_t previous;
    uint64_t first;
    uint64_t max_delta = 0u;
    uint32_t i;
    obc_status_t st;

    *span_out = 0u;

    st = obc_mtime_read(&previous);
    if (st != OBC_OK) {
        return st;
    }
    first = previous;

    for (i = 0u; i < TICK_CHECK_READS; i++) {
        uint64_t current;
        uint64_t delta;

        st = obc_mtime_read(&current);
        if (st != OBC_OK) {
            return st;
        }

        if (current < previous) {
            *max_delta_out = previous - current;
            return OBC_ERR_INVALID; /* went backwards */
        }

        delta = current - previous;
        if (delta > max_delta) {
            max_delta = delta;
        }
        if (delta >= TICK_DELTA_MAX) {
            *max_delta_out = delta;
            return OBC_ERR_UNSTABLE; /* jumped further than a clock can */
        }

        previous = current;
    }

    *span_out = previous - first;
    *max_delta_out = max_delta;
    return OBC_OK;
}

static obc_status_t print_tick_check(void)
{
    uint64_t max_delta = 0u;
    uint64_t span = 0u;
    obc_status_t st = check_tick_counter(&max_delta, &span);
    obc_status_t w;

    w = obc_uart_puts("tick   : ");
    if (w != OBC_OK) {
        return w;
    }

    if (st == OBC_OK) {
        w = obc_uart_puts("ok, max delta ");
    } else if (st == OBC_ERR_INVALID) {
        w = obc_uart_puts("FAULT went backwards by ");
    } else if (st == OBC_ERR_UNSTABLE) {
        w = obc_uart_puts("FAULT implausible jump of ");
    } else {
        w = obc_uart_puts("FAULT unreadable, code ");
    }
    if (w != OBC_OK) {
        return w;
    }

    /* The delta is 64-bit; report the high word too, since the whole point of
     * the carry defect is that it lands there. */
    w = obc_uart_put_hex32((uint32_t)(max_delta >> 32));
    if (w != OBC_OK) {
        return w;
    }
    w = obc_uart_puts(":");
    if (w != OBC_OK) {
        return w;
    }
    w = obc_uart_put_hex32((uint32_t)max_delta);
    if (w != OBC_OK) {
        return w;
    }
    w = obc_uart_puts(" ticks\r\n");
    if (w != OBC_OK) {
        return w;
    }

    /*
     * Stated as reads and ticks, never rounded up into a duration. "10^6 reads
     * covering N ticks" is what was measured; "the clock was stable for X
     * seconds" would be a claim about elapsed time that this check does not
     * make and, under -icount, could not make honestly.
     */
    w = obc_uart_puts("reads  : ");
    if (w != OBC_OK) {
        return w;
    }
    w = obc_uart_put_u32((uint32_t)TICK_CHECK_READS);
    if (w != OBC_OK) {
        return w;
    }
    w = obc_uart_puts(" covering ");
    if (w != OBC_OK) {
        return w;
    }
    w = obc_uart_put_u32((uint32_t)span);
    if (w != OBC_OK) {
        return w;
    }
    w = obc_uart_puts(" ticks\r\n");
    if (w != OBC_OK) {
        return w;
    }

    return st;
}

/*
 * Reports the reset cause left by the previous boot, then clears it.
 *
 * The cause is stored as a full 32-bit mcause and reported that way. Bit 31
 * separates an interrupt from an exception, and code 3 means "breakpoint" as an
 * exception and "machine software interrupt" as an interrupt — unrelated
 * events. Reporting the code alone would make them the same line in a log, and
 * that ambiguity would cost a night during the first real anomaly.
 */
static obc_status_t print_reset_cause(void)
{
    uint32_t reset_cause = OBC_RESET_UNKNOWN;
    obc_status_t v = obc_fault_validate(&reset_cause);
    obc_status_t w;

    w = obc_uart_puts("reset  : ");
    if (w != OBC_OK) {
        return w;
    }

    if (v == OBC_ERR_INVALID) {
        return obc_uart_puts("none recorded (cold boot or torn write)\r\n");
    }
    if (v == OBC_ERR_UNSTABLE) {
        return obc_uart_puts("RECORD CORRUPT: magic valid, checksum wrong\r\n");
    }
    if (reset_cause == OBC_RESET_DOUBLE_FAULT) {
        return obc_uart_puts("DOUBLE FAULT (no detail recorded, by design)\r\n");
    }

    w = obc_uart_puts("trap mcause=");
    if (w == OBC_OK) {
        w = obc_uart_put_hex32(obc_fault_record.cause);
    }
    if (w == OBC_OK) {
        w = obc_uart_puts(obc_mcause_is_interrupt(obc_fault_record.cause)
                              ? " (interrupt " : " (exception ");
    }
    if (w == OBC_OK) {
        w = obc_uart_put_u32(obc_mcause_code(obc_fault_record.cause));
    }
    if (w == OBC_OK) {
        w = obc_uart_puts(") epc=");
    }
    if (w == OBC_OK) {
        w = obc_uart_put_hex32(obc_fault_record.epc);
    }
    if (w == OBC_OK) {
        w = obc_uart_puts(" tval=");
    }
    if (w == OBC_OK) {
        w = obc_uart_put_hex32(obc_fault_record.tval);
    }
    if (w == OBC_OK) {
        w = obc_uart_puts("\r\n");
    }
    return w;
}

/*
 * The trap handler reaches the fault record only through mscratch. If that
 * register is wrong, every fault from here on is written to the wrong address
 * and the failure is silent. The startup code sets it four instructions in;
 * this checks it rather than trusting the position of those instructions.
 */
static obc_status_t print_mscratch_invariant(void)
{
    uint32_t mscratch;
    uint32_t expected = (uint32_t)(uintptr_t)&obc_fault_record;
    obc_status_t w;

    __asm__ volatile("csrr %0, mscratch" : "=r"(mscratch));

    w = obc_uart_puts("mscratch: ");
    if (w != OBC_OK) {
        return w;
    }
    w = obc_uart_put_hex32(mscratch);
    if (w != OBC_OK) {
        return w;
    }
    if (mscratch == expected) {
        return obc_uart_puts(" ok\r\n");
    }
    w = obc_uart_puts(" FAULT expected ");
    if (w == OBC_OK) {
        w = obc_uart_put_hex32(expected);
    }
    if (w == OBC_OK) {
        w = obc_uart_puts("\r\n");
    }
    return (w == OBC_OK) ? OBC_ERR_INVALID : w;
}

void obc_main(void)
{
    obc_status_t st;
    obc_status_t tick_st;
    obc_status_t inv_st;

    obc_uart_init();

    /*
     * The banner is the only observable this milestone has. If a write fails
     * there is nowhere left to report it: the UART *is* the reporting channel.
     * The status is still checked rather than discarded, so that the failure
     * path exists and is visible when M1 gives it somewhere to go.
     */
    st = obc_uart_puts("\r\n=== OBC-Zero ===\r\n");
    if (st == OBC_OK) {
        st = obc_uart_puts("build  : ");
    }
    if (st == OBC_OK) {
        st = obc_uart_puts(build_hash);
    }
    if (st == OBC_OK) {
        st = obc_uart_puts("\r\n");
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

    inv_st = print_mscratch_invariant();
    if (st == OBC_OK) {
        st = print_reset_cause();
    }
    obc_fault_consume();

    tick_st = print_tick_check();

    /*
     * Sentinel. This exact string is what the host watches for to decide that
     * the run has reached its end state; seeing it, the harness stops QEMU
     * rather than waiting out a timeout. Changing it breaks `make test`.
     *
     * A failed self-check still reaches an end state, and must still say so:
     * a run that goes silent is indistinguishable from a hang, which would
     * hide the very fault the check just found. The sentinel therefore reports
     * the verdict rather than being withheld on failure.
     */
    if (st == OBC_OK && tick_st == OBC_OK && inv_st == OBC_OK) {
        (void)obc_uart_puts("boot   : ok\r\n");
    } else {
        (void)obc_uart_puts("boot   : FAULT\r\n");
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
