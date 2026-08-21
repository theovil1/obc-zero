/*
 * Cyclic executive.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/sched.h"

#include <stdint.h>

#include "core/mode.h"
#include "core/status.h"
#include "hal/mtime.h"

volatile uint8_t obc_trace[OBC_TRACE_CAPACITY];
volatile uint32_t obc_trace_len;
volatile uint32_t obc_trace_overflow;

obc_task_state_t obc_task_state[OBC_MAX_TASKS];

volatile uint32_t obc_frames_run;
volatile uint32_t obc_frame_overruns;
volatile uint32_t obc_slack_ticks_min;
volatile uint32_t obc_window_start_ticks;
volatile uint32_t obc_window_end_ticks;
volatile uint32_t obc_safe_entry_frame = OBC_SAFE_ENTRY_NONE;

static inline uint32_t read_minstret(void)
{
    uint32_t v;
    __asm__ volatile("csrr %0, minstret" : "=r"(v));
    return v;
}

static void trace_push(uint32_t task_index)
{
    if (obc_trace_len >= OBC_TRACE_CAPACITY) {
        /*
         * Counted rather than wrapped. A ring buffer would silently change what
         * the ordering assertion compares, turning a window that overflowed
         * into one that merely looks different. An overflow is a test that was
         * asked to observe more than it can hold, and it must fail loudly.
         */
        obc_trace_overflow++;
        return;
    }
    obc_trace[obc_trace_len] = (uint8_t)task_index;
    obc_trace_len++;
}

/*
 * Dispatches one task and accounts for it.
 *
 * The budget is checked on return, which is after the task has already spent
 * whatever it spent. That is a decision, not an oversight: see ADR 0003. An
 * overrun in a system with a static table and no dynamic allocation is a design
 * defect to be found in a campaign and fixed, not a runtime condition to be
 * handled, and preventing one would require preempting a task that did not
 * yield — which works against the M1 rule that a handler must not touch the
 * stack.
 */
static void dispatch(uint32_t index)
{
    const obc_task_t *task = &obc_task_table[index];
    obc_task_state_t *state = &obc_task_state[index];
    uint32_t before;
    uint32_t used;

    if (task->fn == 0) {
        return; /* a null function pointer is never called */
    }

    trace_push(index);

    before = read_minstret();
    task->fn();
    /* Unsigned subtraction is wrap-safe, which matters because minstret is read
     * 32 bits wide and a long campaign will wrap it. */
    used = read_minstret() - before;

    state->runs++;
    if (used > state->max_instr) {
        state->max_instr = used;
    }
    if (used > task->budget_instr) {
        state->overruns++;
    }
}

/*
 * Reads the clock, degrading if it will not settle.
 *
 * One place rather than three. The first version wired the clock entry point on
 * a single one of the executive's three obc_mtime_read call sites, and the
 * failure landed on one of the other two — so the executive returned an error
 * without ever degrading. The property "reachable from three subsystems" is not
 * satisfied by an entry point that only works if the failure is polite about
 * where it happens.
 */
static obc_status_t sched_now(uint64_t *out, uint32_t frame)
{
    obc_status_t st = obc_mtime_read(out);

    if (st == OBC_ERR_UNSTABLE && !obc_mode_is_safe()) {
        obc_safe_entry_frame = frame;
        obc_mode_enter_safe(OBC_SAFE_CLOCK);
    }
    return st;
}

obc_status_t obc_sched_run(uint32_t frames)
{
    uint64_t frame_start;
    uint32_t frame;
    obc_status_t st;

    if (obc_task_count == 0u || obc_task_count > OBC_MAX_TASKS) {
        return OBC_ERR_INVALID;
    }

    st = sched_now(&frame_start, 0u);
    if (st != OBC_OK) {
        return st;
    }

    obc_slack_ticks_min = OBC_FRAME_TICKS;
    obc_window_start_ticks = (uint32_t)frame_start;
    if (obc_mode_is_safe() && obc_safe_entry_frame == OBC_SAFE_ENTRY_NONE) {
        /* Already degraded on entry, restored from the previous boot. Frame 0
         * so the host holds the whole window to the essential subset. */
        obc_safe_entry_frame = 0u;
    }

    for (frame = 0u; frame < frames; frame++) {
        uint64_t deadline = frame_start + OBC_FRAME_TICKS;
        uint64_t now;
        uint32_t i;
        uint32_t guard;

        /*
         * Dispatch every task due this frame, in table order. Order is a
         * property the assertions check position by position, so it is fixed by
         * the table and never by arrival time or by any dynamic priority.
         */
        for (i = 0u; i < obc_task_count; i++) {
            if (obc_task_table[i].period_frames == 0u) {
                continue;
            }
            /* Degraded: only the essential subset is dispatched. The cadence is
             * unchanged — a degraded system that also moved its timebase would
             * be two failures to reason about instead of one. */
            if (obc_mode_is_safe() && obc_task_table[i].essential == 0u) {
                continue;
            }
            if ((frame % obc_task_table[i].period_frames) == 0u) {
                dispatch(i);
            }
        }

        /* Slack: ticks left between the last dispatch and the frame end. A
         * frame with no slack is a frame about to overrun. */
        /* Entry point 3 of 3: the clock. A timer that will not settle is a
         * machine fault, and continuing to schedule against it would mean
         * trusting deadlines derived from a value the reader rejected. */
        st = sched_now(&now, frame);
        if (st != OBC_OK) {
            return st;
        }
        if (now >= deadline) {
            obc_frame_overruns++;
            obc_slack_ticks_min = 0u;
            /* Entry point 2 of 3: the executive itself. Direct, with no reset —
             * nothing is wrong with the machine, only with what it was asked to
             * do. */
            if (!obc_mode_is_safe()) {
                obc_safe_entry_frame = frame;
                obc_mode_enter_safe(OBC_SAFE_FRAME_OVERRUN);
            }
        } else {
            uint32_t slack = (uint32_t)(deadline - now);
            if (slack < obc_slack_ticks_min) {
                obc_slack_ticks_min = slack;
            }
        }

        /*
         * Idle to the frame boundary. Bounded by a guard rather than trusting
         * the clock: an mtime that stopped advancing would otherwise spin here
         * forever, and this is the one loop in the executive where that could
         * happen.
         */
        for (guard = 0u; guard < (OBC_FRAME_TICKS * 8192u); guard++) {
            st = sched_now(&now, frame);
            if (st != OBC_OK) {
                return st;
            }
            if (now >= deadline) {
                break;
            }
        }
        if (now < deadline) {
            return OBC_ERR_TIMEOUT; /* the clock did not reach the deadline */
        }

        /*
         * Advance on the absolute schedule rather than from "now". A frame that
         * ran long must not drag the whole cadence with it, because the
         * conformance assertion counts dispatches against the table's periods
         * and a drifting cadence would silently change that count.
         */
        frame_start = deadline;
        obc_frames_run++;
    }

    obc_window_end_ticks = (uint32_t)frame_start;
    return OBC_OK;
}
