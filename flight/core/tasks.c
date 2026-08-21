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

#include "core/critical.h"
#include "core/recover.h"
#include "core/sched.h"
#include "core/status.h"
#include "hal/uart.h"
#include "tlm/frame.h"
#include "tlm/sensor.h"

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
 * Essentiality. Re-examined at M6, as ADR 0005 said it would be, and the answer
 * changed: telemetry is now essential.
 *
 * A degraded system that stops describing itself is a system nobody can
 * diagnose, and safe mode is exactly the state in which somebody most needs to
 * know what happened. Suspending telemetry to save work in a degraded system
 * saves very little and costs the only channel through which the degradation is
 * visible. Recorded in ADR 0008 decision 2.
 */
#define T0_ESSENTIAL 1u
#define T1_ESSENTIAL 1u
#define T2_ESSENTIAL 0u
#define T3_ESSENTIAL 0u

/*
 * Budgets, in retired instructions.
 *
 * T1 is set from the measured worst dispatch — 1633 instructions to sample two
 * sensors and write 45 bytes — with margin, rather than from an estimate.
 *
 * **T1 is an emulation figure, and the silicon figure is twenty times larger.**
 * QEMU's chardev accepts every byte the instant it is offered, so 1466 measures
 * a UART with no baud rate. At the 115200 the driver configures, one byte is
 * 1356 core instructions and a 45-byte frame is **61035** — 12.5 % of a whole
 * frame, against a budget of 3000.
 *
 * The budget stays at the emulated figure because that is the machine the
 * campaigns run on, and a budget no run can meet would make every campaign red
 * for a reason no campaign can fix. It is labelled rather than trusted, so that
 * nobody ports this and discovers the gap at integration.
 *
 * **What the port has to change is not this number.** Polling 61035 instructions
 * inside a dispatch is not a budget that is too small, it is the wrong shape:
 * the transmit has to become non-blocking, with the frame handed to a driver
 * that drains it across frames. ADR 0009, decision 4.
 */
#define T0_BUDGET 1000u
#define T1_BUDGET 3000u

/*
 * The telemetry task's nominal cost — a dispatch in which the downlink never
 * refuses — measured on this build and re-measured by every `make measure`
 * through the max the executive reports. Named so the assertion below can reason
 * about it instead of repeating a literal.
 */
#define T1_NOMINAL_INSTR 1466u
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

/*
 * Telemetry: sample every sensor, then pack and emit one housekeeping frame.
 *
 * Both statuses are discarded here, and each discard is a claim that there is
 * genuinely nowhere for it to go:
 *
 * - a rejected reading is already published, in the frame's flag byte and in the
 *   substitute value that replaced it. Returning it would tell a caller that
 *   does not exist something the frame already says.
 * - an emission failure has no channel left to be reported on. The UART *is* the
 *   reporting channel, which is the same position main()'s banner is in. The
 *   host sees it as a frame that never arrived or one whose sum does not check.
 *
 * The escalation path for a telemetry task that misbehaves is the same as for
 * every other task: it overruns its budget, and the ladder in dispatch() climbs.
 * Giving this task its own private route to escalation would be a second policy
 * beside the one ADR 0007 defined.
 */
static void task_telemetry(void)
{
    OBC_IGNORE(obc_sensor_sample());
    OBC_IGNORE(obc_tlm_emit());
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

/*
 * The table. The last column is the task's subsystem reset — rung 2 of the
 * escalation ladder — and it is NULL for every task that owns no state of its
 * own. Those tasks escalate from rung 1 straight to rung 3, because a rung that
 * calls nothing is worse than a rung that is absent.
 */
const obc_task_t obc_task_table[] = {
    { "housekeeping", task_housekeeping, T0_PERIOD, T0_BUDGET, T0_ESSENTIAL, 0 },
    { "telemetry",    task_telemetry,    T1_PERIOD, T1_BUDGET, T1_ESSENTIAL,
      obc_tlm_subsystem_reset },
    { "scrub",        task_scrub,        T2_PERIOD, T2_BUDGET, T2_ESSENTIAL, 0 },
    { "audit",        task_audit,        T3_PERIOD, T3_BUDGET, T3_ESSENTIAL, 0 },
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

/*
 * The emission's retry allowance has to fit inside the budget that governs it,
 * with the task's nominal work already paid for.
 *
 * This is the assertion the previous version of the UART had no equivalent of.
 * Its bound was 100000 polls per byte — a number unrelated to any budget, in a
 * loop inside a budgeted task — and nothing anywhere could notice. See
 * docs/adr/0009-emission-under-refusal.md.
 *
 * Three measured figures meet here: the budget, the nominal dispatch cost, and
 * the cost of one poll taken from the disassembly. Move any of them until they
 * no longer fit and this fails at build time.
 */
_Static_assert(T1_NOMINAL_INSTR
                       + (OBC_UART_TX_RETRY_TOTAL * OBC_UART_TX_POLL_INSTR)
                   <= T1_BUDGET,
               "a stalled downlink would overrun the telemetry budget, so the "
               "emission could reset the machine over a congested link");

/*
 * And the nominal cost must itself be inside the budget, which is not implied by
 * the above the day someone raises the allowance and lowers the nominal figure
 * to make it fit.
 */
_Static_assert(T1_NOMINAL_INSTR < T1_BUDGET,
               "the telemetry task's measured cost does not fit its own budget");

/* Every period must divide the window exactly, or the expected count is not an
 * integer and the conformance assertion would need a tolerance. */
_Static_assert((OBC_ASSERT_WINDOW_FRAMES % T0_PERIOD) == 0u
                   && (OBC_ASSERT_WINDOW_FRAMES % T1_PERIOD) == 0u
                   && (OBC_ASSERT_WINDOW_FRAMES % T2_PERIOD) == 0u
                   && (OBC_ASSERT_WINDOW_FRAMES % T3_PERIOD) == 0u,
               "a period that does not divide the window would need a tolerance");
