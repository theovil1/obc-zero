/*
 * DELIBERATELY BROKEN. Not flight code. Never linked into a flight image.
 *
 * A copy of flight/core/tasks.c whose *period-1* task overruns on every
 * dispatch, so the ladder climbs all the way and the system resets on every
 * boot. That is what a reset loop looks like from the inside.
 *
 * The period matters. harness/broken/tasks_overrun.c starves a period-8 task,
 * which reaches rung 1 and stops there: suspended at its next due frame, it
 * never gets a second dispatch inside the window and never reaches rung 3. Once
 * rung 1 actually withheld a dispatch, that variant stopped looping — which is
 * the suspension working, not the ladder failing.
 *
 * A period-1 task overruns in frame 0, is withheld in frame 1, and overruns
 * again in frame 2. Two faults, rung 3, reset, and the next boot repeats it.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include "core/critical.h"
#include "core/recover.h"
#include "core/sched.h"
#include "core/status.h"

/*
 * Periods in frames, and budgets in retired instructions. Named so the static
 * assertions below can reason about them, rather than repeating literals that
 * could drift away from the table.
 *
 * Periods are powers of two so that "N frames" divides exactly by every period.
 * That is what lets the conformance assertion demand an equality: over 16
 * frames a period-4 task runs exactly 4 times, with no remainder to argue
 * about.
 */
#define T0_PERIOD 1u
#define T1_PERIOD 2u
#define T2_PERIOD 4u
#define T3_PERIOD 8u

/*
 * Essentiality. Only housekeeping survives a degraded mode, because its
 * continued dispatch is the only evidence the executive is still running. M6
 * and M7 will re-examine this set once telemetry has a frame and commands have
 * an ingest path; that re-examination is a change to ADR 0005, not a surprise.
 */
#define T0_ESSENTIAL 1u
#define T1_ESSENTIAL 0u
#define T2_ESSENTIAL 0u
#define T3_ESSENTIAL 0u

#define T0_BUDGET 100u /* THE DEFECT: far below the ~258 this task uses */
#define T1_BUDGET 1500u
#define T2_BUDGET 3000u
#define T3_BUDGET 5000u

/* Deterministic work. volatile so the compiler cannot delete the loop, which
 * would make every budget meaningless while every test still passed. */
static volatile uint32_t work_sink;

static void work(uint32_t iterations)
{
    uint32_t i;

    for (i = 0u; i < iterations; i++) {
        work_sink += i;
    }
}

static void task_housekeeping(void)
{
    work(50u);
}

static void task_telemetry(void)
{
    work(100u);
}

/*
 * The scrubber. Real work rather than a stub from here on: it walks the
 * critical copies and repairs a dissenter without anyone needing to read the
 * value. State written once and read rarely would otherwise accumulate
 * corruptions until a second one made the first unrecoverable.
 *
 * The status is discarded deliberately. An unresolved vote is already counted
 * in obc_critical_failed_votes and has already driven the system degraded through
 * obc_mode_is_safe; there is nothing this task can add by returning it, and it
 * has no caller to return it to.
 */
static void task_scrub(void)
{
    OBC_IGNORE(obc_critical_scrub());
}

static void task_audit(void)
{
    work(400u);
}

const obc_task_t obc_task_table[] = {
    { "housekeeping", task_housekeeping, T0_PERIOD, T0_BUDGET, T0_ESSENTIAL },
    { "telemetry",    task_telemetry,    T1_PERIOD, T1_BUDGET, T1_ESSENTIAL },
    { "scrub",        task_scrub,        T2_PERIOD, T2_BUDGET, T2_ESSENTIAL },
    { "audit",        task_audit,        T3_PERIOD, T3_BUDGET, T3_ESSENTIAL },
};

const uint32_t obc_task_count =
    (uint32_t)(sizeof(obc_task_table) / sizeof(obc_task_table[0]));

/* --- Compile-time checks ------------------------------------------------- */

_Static_assert(sizeof(obc_task_table) / sizeof(obc_task_table[0]) <= OBC_MAX_TASKS,
               "more tasks than the trace and state arrays are sized for");

/* A zero period would mean "never", expressed as a division by zero waiting to
 * happen. If a task should not run, remove it from the table. */
_Static_assert(T0_PERIOD > 0u && T1_PERIOD > 0u && T2_PERIOD > 0u && T3_PERIOD > 0u,
               "a task period of zero is not a way to disable a task");

/*
 * The frame where every task is due is the binding one, and with periods that
 * are powers of two it happens whenever the frame index is a multiple of the
 * largest period. Every budget must fit in a single frame simultaneously.
 */
_Static_assert(T0_BUDGET + T1_BUDGET + T2_BUDGET + T3_BUDGET
                   <= OBC_FRAME_INSTR_CAPACITY,
               "the declared budgets cannot all fit in one frame");

/*
 * The window the assertions run over must fit in the trace. Sixteen frames of
 * this table is 16 + 8 + 4 + 2 = 30 dispatches; the check is written against
 * the periods so that changing one is caught here rather than by an overflow
 * counter at run time.
 */
#define OBC_ASSERT_WINDOW_FRAMES OBC_SCHED_WINDOW_FRAMES
_Static_assert((OBC_ASSERT_WINDOW_FRAMES / T0_PERIOD)
                       + (OBC_ASSERT_WINDOW_FRAMES / T1_PERIOD)
                       + (OBC_ASSERT_WINDOW_FRAMES / T2_PERIOD)
                       + (OBC_ASSERT_WINDOW_FRAMES / T3_PERIOD)
                   <= OBC_TRACE_CAPACITY,
               "the assertion window produces more dispatches than the trace holds");

/*
 * At least one task must survive a degraded mode. A safe mode that dispatches
 * nothing is a stopped system wearing a different name, and over a
 * transmit-only serial line it is indistinguishable from a hang.
 */
_Static_assert(T0_ESSENTIAL + T1_ESSENTIAL + T2_ESSENTIAL + T3_ESSENTIAL >= 1u,
               "safe mode would dispatch nothing, which is a halt not a mode");

/*
 * A boot is called healthy after OBC_HEALTHY_FRAMES, and that number is only
 * meaningful if every task has been dispatched by then. A table gaining a
 * slower task must raise it, and this is where that is noticed.
 */
_Static_assert(T0_PERIOD <= OBC_HEALTHY_FRAMES && T1_PERIOD <= OBC_HEALTHY_FRAMES
                   && T2_PERIOD <= OBC_HEALTHY_FRAMES
                   && T3_PERIOD <= OBC_HEALTHY_FRAMES,
               "a boot would be called healthy before every task had run once");

/* Every period must divide the window exactly, or the expected count is not an
 * integer and the conformance assertion would need a tolerance. */
_Static_assert((OBC_ASSERT_WINDOW_FRAMES % T0_PERIOD) == 0u
                   && (OBC_ASSERT_WINDOW_FRAMES % T1_PERIOD) == 0u
                   && (OBC_ASSERT_WINDOW_FRAMES % T2_PERIOD) == 0u
                   && (OBC_ASSERT_WINDOW_FRAMES % T3_PERIOD) == 0u,
               "a period that does not divide the window would need a tolerance");
