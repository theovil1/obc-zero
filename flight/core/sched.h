/*
 * Cyclic executive.
 *
 * The property this exists to make provable is stated in
 * docs/adr/0003-scheduler-observability.md and is worth repeating here, because
 * it is not the obvious one: what must be shown is **not that the tasks run**.
 * It is that the order and the number of executions within a window are exactly
 * those the table dictates, and that they do not vary between runs.
 *
 * A scheduler that runs a task 999 times instead of 1000 produces a system that
 * works, until the missing execution is the one that mattered. That is why the
 * assertions compare against the table rather than against a tolerance.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OBC_SCHED_H
#define OBC_SCHED_H

#include <stdint.h>

#include "core/status.h"
#include "hal/mtime.h"

/*
 * Minor frame, in machine-timer ticks. A power of two on purpose: it makes
 * "N frames" and "every P frames" exact rather than approximately exact, which
 * is what allows the conformance assertion to demand an equality instead of a
 * range.
 *
 * 1024 ticks at 32768 Hz is 31.25 ms.
 */
#define OBC_FRAME_TICKS 1024u

/*
 * Instructions available inside one frame, derived from the ratio measured in
 * ADR 0002 rather than from a guess.
 *
 * This figure is an *emulation* property: it depends on the -icount shift, and
 * on real silicon it depends on the core clock. It is used only for the
 * compile-time check that the declared budgets can fit in a frame at all. It is
 * not a claim about hardware, and ADR 0002 requires the port to recalibrate.
 */
#define OBC_MILLI_INSTR_PER_TICK 476837u
#define OBC_FRAME_INSTR_CAPACITY \
    ((uint32_t)((OBC_MILLI_INSTR_PER_TICK / 1000u) * OBC_FRAME_TICKS))

/* Hard ceiling on the table, so the trace and the state arrays are sized at
 * compile time like everything else here. */
#define OBC_MAX_TASKS 8u

/* The window the milestone's assertions run over. Fixed here so the table's
 * compile-time checks and the executive agree on it by construction. */
#define OBC_SCHED_WINDOW_FRAMES 16u

typedef void (*obc_task_fn)(void);

/*
 * A task, as declared. Entirely const: the table lives in flash, so a corrupted
 * RAM word cannot turn a period into something else or redirect a function
 * pointer. Mutable per-task state lives separately, in obc_task_state_t.
 */
typedef struct {
    const char *name;
    obc_task_fn fn;
    uint32_t period_frames; /* dispatch every N frames; 1 means every frame */
    uint32_t budget_instr;  /* retired instructions allowed per dispatch */
    /*
     * Whether the system still dispatches this task once degraded.
     *
     * An explicit field, never inferred from the period. A period says how
     * often a task runs; it says nothing about whether the system can do
     * without it, and deriving one from the other would make safe mode change
     * silently whenever a cadence was adjusted. See
     * docs/adr/0005-safe-mode.md.
     */
    uint32_t essential;
    /*
     * The task's subsystem reset, or NULL if it owns no state of its own.
     *
     * Rung 2 of ADR 0007's ladder needs to know what to reinitialise, and the
     * escalation code is the wrong place to know it: a switch on task index
     * inside recover.c would be a second table, kept in step with this one by
     * hand. Declaring it here means a task that owns state says so, and a task
     * that does not is skipped past rung 2 rather than given a rung that
     * silently does nothing.
     */
    obc_task_fn reset_fn;
} obc_task_t;

/*
 * Mutable per-task accounting. Separated from the declaration above so that the
 * table can be const, and kept small because it is charged to the M2 line in
 * docs/BUDGET.md.
 */
typedef struct {
    uint32_t runs;
    uint32_t overruns;
    uint32_t max_instr; /* worst dispatch observed, in retired instructions */
} obc_task_state_t;

/*
 * Execution trace: one byte of task index per dispatch, in dispatch order.
 *
 * Only the identity is recorded. Order and count are what the conformance and
 * ordering assertions compare, and per-task instruction figures live in the
 * state array where they cost three words rather than one entry per dispatch.
 *
 * Written to RAM and read out by the host through the debugger. Emitting it
 * over the UART inside a dispatch would add instructions to the very count
 * being asserted.
 *
 * **This is flight code, not instrumentation**, and it is never compiled out.
 * Gating it behind a build flag would mean the campaign measures an image with
 * trace_push in the dispatch path while the vehicle flies one without it, which
 * would make every published budget a figure about a different binary. See
 * docs/adr/0004-trace-is-flight-code.md.
 *
 * **The linear shape is a placeholder.** A buffer that stops recording after
 * OBC_TRACE_CAPACITY dispatches suits a bounded assertion window and is useless
 * in flight, where it would fill within seconds and observe nothing for the rest
 * of the mission. ADR 0004 commits it to becoming a ring with an explicit wrap
 * counter, delivered with the event log at M8.
 */
#define OBC_TRACE_CAPACITY 256u

extern volatile uint8_t obc_trace[OBC_TRACE_CAPACITY];
extern volatile uint32_t obc_trace_len;
extern volatile uint32_t obc_trace_overflow; /* dispatches dropped, must be 0 */

extern obc_task_state_t obc_task_state[OBC_MAX_TASKS];

/* Frame accounting. Slack is in ticks, not instructions: it is a scheduling
 * quantity, and a frame is 1024 ticks so the resolution is ample. Budgets are
 * in instructions because a task can run in less than one tick. */
extern volatile uint32_t obc_frames_run;
extern volatile uint32_t obc_frame_overruns;   /* frames that missed their end */
extern volatile uint32_t obc_slack_ticks_min;  /* worst slack seen */

/*
 * Machine-timer value at the first and last frame boundary of the window.
 *
 * Their difference must be exactly frames x OBC_FRAME_TICKS. That is the
 * assertion which catches an executive that dispatches the right tasks in the
 * right order but does not actually wait out its frames — a scheduler that is
 * correct in every respect the trace can see, and wrong about time.
 */
extern volatile uint32_t obc_window_start_ticks;
extern volatile uint32_t obc_window_end_ticks;

/*
 * The frame at which the executive went degraded, or OBC_SAFE_ENTRY_NONE if it
 * did not. The host needs this to know which part of the trace to hold to the
 * full table and which part to hold to the essential subset only: without it, a
 * degraded run is indistinguishable from a scheduler that dropped dispatches.
 */
/*
 * The clock reading taken at the top of the current frame.
 *
 * Published so the command task can time-tag against the same instant the
 * executive used, rather than reading mtime itself. A second call site would be
 * the shape of the M3 defect exactly: safe mode's clock entry wired to one of
 * three reads, and the failure landing on another.
 */
extern volatile uint32_t obc_cmd_now_ticks;

#define OBC_SAFE_ENTRY_NONE 0xFFFFFFFFu
extern volatile uint32_t obc_safe_entry_frame;

/* The table, defined in flight/core/tasks.c. */
extern const obc_task_t obc_task_table[];
extern const uint32_t obc_task_count;

/*
 * Runs exactly `frames` minor frames, then returns.
 *
 * Bounded by construction: the milestone's assertions are made over a finite
 * window, and an executive that never returns could not be compared between
 * runs. M3 supplies the endless outer loop once there is a recovery policy to
 * put around it.
 */
OBC_MUST_CHECK obc_status_t obc_sched_run(uint32_t frames);

#endif /* OBC_SCHED_H */
