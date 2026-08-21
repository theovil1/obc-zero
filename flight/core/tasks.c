/*
 * The task table.
 *
 * Compile-time constant, in flash, and checked at compile time. A table that
 * cannot meet its own frame is a build failure rather than a campaign result:
 * discovering it in a campaign would mean the campaign was measuring the wrong
 * thing all along.
 *
 * The tasks themselves do deterministic bounded work and nothing else. M2 is
 * about the executive, not about what runs on it; telemetry arrives at M6 and
 * the scrubber at M4, and giving them stubs here would invite reading this
 * table as a design for them.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include "core/sched.h"

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

#define T0_BUDGET 1000u
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

static void task_scrub(void)
{
    work(200u);
}

static void task_audit(void)
{
    work(400u);
}

const obc_task_t obc_task_table[] = {
    { "housekeeping", task_housekeeping, T0_PERIOD, T0_BUDGET },
    { "telemetry",    task_telemetry,    T1_PERIOD, T1_BUDGET },
    { "scrub",        task_scrub,        T2_PERIOD, T2_BUDGET },
    { "audit",        task_audit,        T3_PERIOD, T3_BUDGET },
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

/* Every period must divide the window exactly, or the expected count is not an
 * integer and the conformance assertion would need a tolerance. */
_Static_assert((OBC_ASSERT_WINDOW_FRAMES % T0_PERIOD) == 0u
                   && (OBC_ASSERT_WINDOW_FRAMES % T1_PERIOD) == 0u
                   && (OBC_ASSERT_WINDOW_FRAMES % T2_PERIOD) == 0u
                   && (OBC_ASSERT_WINDOW_FRAMES % T3_PERIOD) == 0u,
               "a period that does not divide the window would need a tolerance");
