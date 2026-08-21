/*
 * DELIBERATELY BROKEN. Not flight code. Never linked into a flight image.
 *
 * A scheduler that drops exactly one dispatch, once, in the middle of the
 * window. Everything else is identical to flight/core/sched.c.
 *
 * This is the defect the milestone exists to make detectable, and it is chosen
 * to be as unremarkable as possible: the system boots, every task runs, the
 * frames are waited out, the slack is healthy, and the serial log looks
 * correct. Only the count and the order betray it. A scheduler wrong in exactly
 * this way would run for months.
 *
 * **This is a copy, not a variant.** It duplicates the dispatch loop rather than
 * hooking into the real one, because a hook would mean test scaffolding inside
 * flight/. If flight/core/sched.c changes shape, this file must be re-synced —
 * and `make test-sched-broken` failing to detect anything is what will say so.
 *
 * Built only by `make test-sched-broken`, which asserts the checker REJECTS it.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/sched.h"

#include <stdint.h>

#include "core/status.h"
#include "hal/mtime.h"

/* The one frame on which task 0 is quietly not dispatched. */
#define BROKEN_SKIP_FRAME 7u
#define BROKEN_SKIP_TASK 0u

volatile uint8_t obc_trace[OBC_TRACE_CAPACITY];
volatile uint32_t obc_trace_len;
volatile uint32_t obc_trace_overflow;

obc_task_state_t obc_task_state[OBC_MAX_TASKS];

volatile uint32_t obc_frames_run;
volatile uint32_t obc_frame_overruns;
volatile uint32_t obc_slack_ticks_min;
volatile uint32_t obc_window_start_ticks;
volatile uint32_t obc_window_end_ticks;

static inline uint32_t read_minstret(void)
{
    uint32_t v;
    __asm__ volatile("csrr %0, minstret" : "=r"(v));
    return v;
}

static void trace_push(uint32_t task_index)
{
    if (obc_trace_len >= OBC_TRACE_CAPACITY) {
        obc_trace_overflow++;
        return;
    }
    obc_trace[obc_trace_len] = (uint8_t)task_index;
    obc_trace_len++;
}

static void dispatch(uint32_t index)
{
    const obc_task_t *task = &obc_task_table[index];
    obc_task_state_t *state = &obc_task_state[index];
    uint32_t before;
    uint32_t used;

    if (task->fn == 0) {
        return;
    }

    trace_push(index);

    before = read_minstret();
    task->fn();
    used = read_minstret() - before;

    state->runs++;
    if (used > state->max_instr) {
        state->max_instr = used;
    }
    if (used > task->budget_instr) {
        state->overruns++;
    }
}

obc_status_t obc_sched_run(uint32_t frames)
{
    uint64_t frame_start;
    uint32_t frame;
    obc_status_t st;

    if (obc_task_count == 0u || obc_task_count > OBC_MAX_TASKS) {
        return OBC_ERR_INVALID;
    }

    st = obc_mtime_read(&frame_start);
    if (st != OBC_OK) {
        return st;
    }

    obc_slack_ticks_min = OBC_FRAME_TICKS;
    obc_window_start_ticks = (uint32_t)frame_start;

    for (frame = 0u; frame < frames; frame++) {
        uint64_t deadline = frame_start + OBC_FRAME_TICKS;
        uint64_t now;
        uint32_t i;
        uint32_t guard;

        for (i = 0u; i < obc_task_count; i++) {
            if (obc_task_table[i].period_frames == 0u) {
                continue;
            }
            /* THE DEFECT. One dispatch, once, and nothing else is disturbed. */
            if (frame == BROKEN_SKIP_FRAME && i == BROKEN_SKIP_TASK) {
                continue;
            }
            if ((frame % obc_task_table[i].period_frames) == 0u) {
                dispatch(i);
            }
        }

        st = obc_mtime_read(&now);
        if (st != OBC_OK) {
            return st;
        }
        if (now >= deadline) {
            obc_frame_overruns++;
            obc_slack_ticks_min = 0u;
        } else {
            uint32_t slack = (uint32_t)(deadline - now);
            if (slack < obc_slack_ticks_min) {
                obc_slack_ticks_min = slack;
            }
        }

        for (guard = 0u; guard < (OBC_FRAME_TICKS * 8192u); guard++) {
            st = obc_mtime_read(&now);
            if (st != OBC_OK) {
                return st;
            }
            if (now >= deadline) {
                break;
            }
        }
        if (now < deadline) {
            return OBC_ERR_TIMEOUT;
        }

        frame_start = deadline;
        obc_frames_run++;
    }

    obc_window_end_ticks = (uint32_t)frame_start;
    return OBC_OK;
}
