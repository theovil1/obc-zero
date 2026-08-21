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
#include "core/sched.h"
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

/*
 * Timebase assertion.
 *
 * The criterion this satisfies asks that a change of machine model cannot
 * silently rescale every deadline. It deliberately does NOT time an interval
 * against the host clock, and the reason matters: under -icount guest time
 * advances with instructions executed, not with the wall clock, so a host-timed
 * measurement reports emulation speed and host load rather than the timebase.
 * The number it produced would move with the machine that ran it.
 *
 * The deterministic anchor is the ratio between the two clocks the system
 * already has. Under -icount a continuously executing core retires exactly
 * (10^9 / 2^shift) / OBC_MTIME_HZ instructions per tick: 476.837 at shift=6.
 * That figure changes if the timer frequency changes, if the shift changes, or
 * if the machine changes — which is the whole set of events the criterion is
 * guarding against.
 *
 * What it does not do is establish 32768 Hz in real-time terms. Under -icount
 * there is no real time to compare against. The absolute figure rests on a
 * host-timed measurement taken without -icount, recorded in ADR 0001, and it
 * cannot be re-asserted from inside the guest. Stated here rather than left for
 * someone to assume this check proves more than it does.
 */
#define OBC_MILLI_INSTR_TOLERANCE 4768u /* 1 percent */
#define TIMEBASE_CHECK_TICKS 500u

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

static inline uint32_t read_minstret(void)
{
    uint32_t v;
    __asm__ volatile("csrr %0, minstret" : "=r"(v));
    return v;
}

/*
 * Instructions retired per machine-timer tick, in thousandths.
 *
 * 32-bit arithmetic throughout: a 64-bit division would pull __udivdi3 out of
 * libgcc, which a freestanding link does not provide. The build fails loudly
 * rather than silently, but the constraint is worth stating where it bites.
 */
static obc_status_t measure_instr_per_tick(uint32_t *out_milli)
{
    uint64_t t0;
    uint64_t now;
    uint32_t i0;
    uint32_t dticks;
    uint32_t dinstr;
    uint32_t guard;
    obc_status_t st;

    st = obc_mtime_read(&t0);
    if (st != OBC_OK) {
        return st;
    }
    i0 = read_minstret();

    /* Bounded: the loop cannot legitimately need more iterations than the
     * instruction budget for the span it waits on. */
    for (guard = 0u; guard < (TIMEBASE_CHECK_TICKS * 4096u); guard++) {
        st = obc_mtime_read(&now);
        if (st != OBC_OK) {
            return st;
        }
        if ((uint32_t)(now - t0) >= TIMEBASE_CHECK_TICKS) {
            dinstr = read_minstret() - i0;
            dticks = (uint32_t)(now - t0);
            *out_milli = (dinstr / dticks) * 1000u + ((dinstr % dticks) * 1000u) / dticks;
            return OBC_OK;
        }
    }

    return OBC_ERR_TIMEOUT;
}

static obc_status_t print_timebase_check(void)
{
    uint32_t milli = 0u;
    obc_status_t st = measure_instr_per_tick(&milli);
    uint32_t low = OBC_MILLI_INSTR_PER_TICK - OBC_MILLI_INSTR_TOLERANCE;
    uint32_t high = OBC_MILLI_INSTR_PER_TICK + OBC_MILLI_INSTR_TOLERANCE;
    obc_status_t w;

    w = obc_uart_puts("base   : ");
    if (w != OBC_OK) {
        return w;
    }
    if (st != OBC_OK) {
        (void)obc_uart_puts("FAULT unmeasurable\r\n");
        return st;
    }
    w = obc_uart_put_u32(milli);
    if (w == OBC_OK) {
        w = obc_uart_puts(" milli-instr/tick, expect ");
    }
    if (w == OBC_OK) {
        w = obc_uart_put_u32(OBC_MILLI_INSTR_PER_TICK);
    }
    if (w != OBC_OK) {
        return w;
    }
    if (milli < low || milli > high) {
        /*
         * Name both hypotheses. The ratio is fixed jointly by the timer
         * frequency and by the -icount shift, so it cannot tell them apart —
         * and the nominal reason for it to move is someone deliberately
         * changing the shift, not the timer going wrong. A message blaming the
         * timer alone would send the next person hunting a regression that
         * does not exist.
         */
        (void)obc_uart_puts(" FAULT out of tolerance\r\n");
        (void)obc_uart_puts("         either -icount shift is not 6 "
                            "(the usual cause, and deliberate)\r\n");
        (void)obc_uart_puts("         or the machine timer is not 32768 Hz "
                            "(a real regression)\r\n");
        return OBC_ERR_INVALID;
    }
    return obc_uart_puts(" ok\r\n");
}

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
    if (reset_cause == OBC_RESET_REQUESTED) {
        return obc_uart_puts("requested (watchdog, nothing was wrong)\r\n");
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

/*
 * Reports the executive's outcome. The detail the assertions rely on is read
 * from RAM by the host through the debugger; what is printed here is a summary
 * for a human reading a serial log, and a sentinel-bearing verdict.
 */
static obc_status_t print_sched_summary(obc_status_t run_status)
{
    uint32_t i;
    obc_status_t w;

    w = obc_uart_puts("sched  : ");
    if (w != OBC_OK) {
        return w;
    }
    if (run_status != OBC_OK) {
        (void)obc_uart_puts("FAULT executive returned an error\r\n");
        return run_status;
    }

    w = obc_uart_put_u32(obc_frames_run);
    if (w == OBC_OK) {
        w = obc_uart_puts(" frames, ");
    }
    if (w == OBC_OK) {
        w = obc_uart_put_u32(obc_trace_len);
    }
    if (w == OBC_OK) {
        w = obc_uart_puts(" dispatches, min slack ");
    }
    if (w == OBC_OK) {
        w = obc_uart_put_u32(obc_slack_ticks_min);
    }
    if (w == OBC_OK) {
        w = obc_uart_puts(" ticks\r\n");
    }
    if (w != OBC_OK) {
        return w;
    }

    for (i = 0u; i < obc_task_count; i++) {
        w = obc_uart_puts("  task ");
        if (w == OBC_OK) {
            w = obc_uart_puts(obc_task_table[i].name);
        }
        if (w == OBC_OK) {
            w = obc_uart_puts(" runs=");
        }
        if (w == OBC_OK) {
            w = obc_uart_put_u32(obc_task_state[i].runs);
        }
        if (w == OBC_OK) {
            w = obc_uart_puts(" max=");
        }
        if (w == OBC_OK) {
            w = obc_uart_put_u32(obc_task_state[i].max_instr);
        }
        if (w == OBC_OK) {
            w = obc_uart_puts("/");
        }
        if (w == OBC_OK) {
            w = obc_uart_put_u32(obc_task_table[i].budget_instr);
        }
        if (w == OBC_OK) {
            w = obc_uart_puts(" over=");
        }
        if (w == OBC_OK) {
            w = obc_uart_put_u32(obc_task_state[i].overruns);
        }
        if (w == OBC_OK) {
            w = obc_uart_puts("\r\n");
        }
        if (w != OBC_OK) {
            return w;
        }
    }

    /* An overflowed trace means the window produced more dispatches than the
     * buffer holds, so the ordering assertion would be comparing a truncated
     * sequence. That is a failed test, not a warning. */
    if (obc_trace_overflow != 0u) {
        (void)obc_uart_puts("  TRACE OVERFLOW, the window is larger than the buffer\r\n");
        return OBC_ERR_INVALID;
    }
    if (obc_frame_overruns != 0u) {
        (void)obc_uart_puts("  FRAME OVERRUN\r\n");
        return OBC_ERR_UNSTABLE;
    }
    return OBC_OK;
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
    if (tick_st == OBC_OK) {
        tick_st = print_timebase_check();
    }

    /*
     * Run the executive for a fixed window, then report. A finite window is
     * what makes the trace comparable between runs; the endless outer loop
     * arrives at M3 with a recovery policy to put around it.
     */
    if (st == OBC_OK && tick_st == OBC_OK) {
        obc_status_t sch = obc_sched_run(OBC_SCHED_WINDOW_FRAMES);
        st = print_sched_summary(sch);
    }

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
