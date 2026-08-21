/*
 * Frame packing and emission.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tlm/frame.h"

#include <stdint.h>

#include "core/critical.h"
#include "core/fault.h"
#include "core/mode.h"
#include "core/recover.h"
#include "core/sched.h"
#include "core/status.h"
#include "hal/mtime.h"
#include "hal/uart.h"
#include "tlm/sensor.h"

volatile uint8_t obc_tlm_frame[OBC_TLM_FRAME_LEN];
volatile uint32_t obc_tlm_seq;
volatile uint32_t obc_tlm_subsystem_resets;
volatile uint32_t obc_tlm_drops;
volatile uint32_t obc_tlm_drops_consecutive;

/*
 * Set only by a descriptor audit that passed.
 *
 * A failed audit is a build defect — a table row naming the wrong offset macro —
 * and nothing in flight can repair it. What flight code can do is refuse to
 * emit: a frame packed against a layout the host will decode differently
 * produces plausible wrong numbers, and a campaign would report them as
 * measurements. Silence is recoverable; confident wrong numbers are not.
 */
static uint32_t s_layout_ok;

/*
 * The layout. This table is the definition; the host reads it out of the ELF
 * rather than keeping its own copy, so there is no second list to drift.
 *
 * Offsets are the chained macros from the header, never literals. A row naming
 * the wrong macro is the failure obc_tlm_init() exists to catch — chaining makes
 * the offsets contiguous by construction, it does not make the rows right.
 */
OBC_TLM_KEEP const obc_tlm_field_t obc_tlm_fields[] = {
    { "sync", OBC_TLM_O_SYNC, OBC_TLM_W_SYNC, 1u, OBC_TLM_KIND_SYNC, { 0, 0, 0 } },
    { "seq", OBC_TLM_O_SEQ, OBC_TLM_W_SEQ, 1u, OBC_TLM_KIND_COUNT, { 0, 0, 0 } },
    { "uptime_ticks", OBC_TLM_O_UPTIME, OBC_TLM_W_UPTIME, 1u, OBC_TLM_KIND_COUNT,
      { 0, 0, 0 } },
    { "frames_run", OBC_TLM_O_FRAMES, OBC_TLM_W_FRAMES, 1u, OBC_TLM_KIND_COUNT,
      { 0, 0, 0 } },
    { "frame_overruns", OBC_TLM_O_FRAME_OVERRUNS, OBC_TLM_W_FRAME_OVERRUNS, 1u,
      OBC_TLM_KIND_COUNT, { 0, 0, 0 } },
    { "slack_ticks_min", OBC_TLM_O_SLACK, OBC_TLM_W_SLACK, 1u, OBC_TLM_KIND_COUNT,
      { 0, 0, 0 } },
    { "task_overruns", OBC_TLM_O_TASK_OVERRUNS, OBC_TLM_W_TASK_OVERRUNS,
      OBC_TLM_N_TASK_OVERRUNS, OBC_TLM_KIND_COUNT, { 0, 0, 0 } },
    { "voter_repairs", OBC_TLM_O_REPAIRS, OBC_TLM_W_REPAIRS, 1u, OBC_TLM_KIND_COUNT,
      { 0, 0, 0 } },
    { "voter_failed_votes", OBC_TLM_O_FAILED_VOTES, OBC_TLM_W_FAILED_VOTES, 1u,
      OBC_TLM_KIND_COUNT, { 0, 0, 0 } },
    { "suspensions", OBC_TLM_O_SUSPENSIONS, OBC_TLM_W_SUSPENSIONS, 1u,
      OBC_TLM_KIND_COUNT, { 0, 0, 0 } },
    { "frames_dropped", OBC_TLM_O_DROPS, OBC_TLM_W_DROPS, 1u, OBC_TLM_KIND_COUNT,
      { 0, 0, 0 } },
    { "short_boots", OBC_TLM_O_SHORT_BOOTS, OBC_TLM_W_SHORT_BOOTS, 1u,
      OBC_TLM_KIND_COUNT, { 0, 0, 0 } },
    { "mode", OBC_TLM_O_MODE, OBC_TLM_W_MODE, 1u, OBC_TLM_KIND_ENUM, { 0, 0, 0 } },
    { "safe_reason", OBC_TLM_O_SAFE_REASON, OBC_TLM_W_SAFE_REASON, 1u,
      OBC_TLM_KIND_ENUM, { 0, 0, 0 } },
    { "reset_cause", OBC_TLM_O_RESET_CAUSE, OBC_TLM_W_RESET_CAUSE, 1u,
      OBC_TLM_KIND_ENUM, { 0, 0, 0 } },
    { "sensor_flags", OBC_TLM_O_SENSOR_FLAGS, OBC_TLM_W_SENSOR_FLAGS, 1u,
      OBC_TLM_KIND_BITS, { 0, 0, 0 } },
    { "sensor", OBC_TLM_O_SENSOR, OBC_TLM_W_SENSOR, OBC_TLM_N_SENSOR,
      OBC_TLM_KIND_SENSOR, { 0, 0, 0 } },
    { "sum", OBC_TLM_O_SUM, OBC_TLM_W_SUM, 1u, OBC_TLM_KIND_SUM, { 0, 0, 0 } },
};

OBC_TLM_KEEP const uint32_t obc_tlm_field_count =
    (uint32_t)(sizeof(obc_tlm_fields) / sizeof(obc_tlm_fields[0]));
OBC_TLM_KEEP const uint32_t obc_tlm_frame_len = OBC_TLM_FRAME_LEN;
OBC_TLM_KEEP const uint32_t obc_tlm_sync = OBC_TLM_SYNC;

/* The frame must be able to carry one byte per task the table can hold. */
_Static_assert(OBC_TLM_N_TASK_OVERRUNS == OBC_MAX_TASKS,
               "the frame would report fewer tasks than the table can hold");

/* One reading per sensor, or the frame describes a system other than this one. */
_Static_assert(OBC_TLM_N_SENSOR == OBC_SENSOR_COUNT,
               "the frame has no room for every sensor");

/* --- byte-level packing -------------------------------------------------- */

/*
 * Explicit little-endian writes at descriptor offsets, never a struct cast.
 *
 * A packed struct would still let the compiler's idea of the layout diverge from
 * the descriptor table's, and the divergence would show up as a size-reference
 * drift with no explanation attached. Here the descriptor is the only layout
 * there is.
 */
static void put_u8(uint32_t offset, uint32_t v)
{
    obc_tlm_frame[offset] = (uint8_t)(v & 0xFFu);
}

static void put_u16(uint32_t offset, uint32_t v)
{
    obc_tlm_frame[offset] = (uint8_t)(v & 0xFFu);
    obc_tlm_frame[offset + 1u] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint32_t offset, uint32_t v)
{
    obc_tlm_frame[offset] = (uint8_t)(v & 0xFFu);
    obc_tlm_frame[offset + 1u] = (uint8_t)((v >> 8) & 0xFFu);
    obc_tlm_frame[offset + 2u] = (uint8_t)((v >> 16) & 0xFFu);
    obc_tlm_frame[offset + 3u] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Saturating, because a counter that wraps inside a narrow field reports a small
 * number for a large problem. 0xFFFF means "at least this many". */
static uint32_t sat16(uint32_t v)
{
    return (v > 0xFFFFu) ? 0xFFFFu : v;
}

static uint32_t sat8(uint32_t v)
{
    return (v > 0xFFu) ? 0xFFu : v;
}

/*
 * Not a CRC, and it does not pretend to be. This catches a corrupted or
 * truncated frame, which is the threat on a line that drops bytes; it makes no
 * claim about a pattern chosen to defeat it, and there is no adversary here to
 * choose one. Same reasoning as the critical-state checksum.
 */
static uint32_t frame_sum(void)
{
    uint32_t sum = 0u;
    uint32_t i;

    for (i = 0u; i < OBC_TLM_O_SUM; i++) {
        sum += obc_tlm_frame[i];
        sum = (sum + (sum >> 16)) & 0xFFFFu;
    }
    return sum ^ 0xFFFFu;
}

/* --- lifecycle ----------------------------------------------------------- */

obc_status_t obc_tlm_init(void)
{
    uint32_t expected = 0u;
    uint32_t i;

    if (obc_tlm_field_count == 0u) {
        return OBC_ERR_INVALID;
    }

    for (i = 0u; i < obc_tlm_field_count; i++) {
        const obc_tlm_field_t *f = &obc_tlm_fields[i];

        if (f->width != 1u && f->width != 2u && f->width != 4u) {
            return OBC_ERR_INVALID;
        }
        if (f->count == 0u) {
            return OBC_ERR_INVALID;
        }
        /* Contiguous and in order. A gap is an unnamed byte the host would
         * decode as part of the wrong field; an overlap is two fields writing
         * the same byte, and the second one wins silently. */
        if (f->offset != expected) {
            return OBC_ERR_INVALID;
        }
        expected += (uint32_t)f->width * (uint32_t)f->count;
    }

    if (expected != OBC_TLM_FRAME_LEN) {
        return OBC_ERR_INVALID;
    }

    /*
     * "At least one task can reach rung 2" is deliberately NOT checked here.
     *
     * It was, in the first version, and it was the wrong layer twice over. It
     * coupled the frame layout to the shape of the task table, so a harness
     * build with a deliberately broken table went degraded for a reason that had
     * nothing to do with telemetry — and two scheduler tests started failing on
     * a property they do not test. And it is not a condition the vehicle can act
     * on: a system in orbit cannot grow a subsystem.
     *
     * It is a property of the flight configuration, so the harness checks it
     * against the flight binary. See test-rung2-reachable.
     */

    obc_sensor_init();
    obc_tlm_seq = 0u;
    for (i = 0u; i < OBC_TLM_FRAME_LEN; i++) {
        obc_tlm_frame[i] = 0u;
    }
    s_layout_ok = 1u;
    return OBC_OK;
}

void obc_tlm_subsystem_reset(void)
{
    uint32_t i;

    /*
     * The subsystem's own state, and only that. The counters describing what
     * the system has lived through — overruns, repairs, suspensions — belong to
     * the subsystems that own them, and a recovery action that erased them
     * would leave a campaign with no way to read why it was needed.
     */
    obc_tlm_seq = 0u;
    for (i = 0u; i < OBC_TLM_FRAME_LEN; i++) {
        obc_tlm_frame[i] = 0u;
    }
    obc_sensor_init();
    obc_tlm_subsystem_resets++;
    /* s_layout_ok is deliberately untouched. Rung 2 returns runtime state to a
     * known value; it does not re-litigate a build defect, and a reset that
     * cleared the refusal would turn a permanent fault into an intermittent
     * one. */
}

/* --- emission ------------------------------------------------------------ */

obc_status_t obc_tlm_emit(void)
{
    obc_status_t st;
    uint64_t now = 0u;
    uint32_t uptime;
    uint32_t i;

    /* The refusal, at the only point where it can still take effect. */
    if (s_layout_ok == 0u) {
        return OBC_ERR_INVALID;
    }

    /*
     * A failed clock read publishes an unmistakable value rather than the
     * previous frame's uptime. Repeating the last good reading would put a
     * plausible number in the frame and let every consumer carry on, which is
     * what "propagated" means. The read failure is separately counted by
     * obc_mtime_unstable_count and drives safe mode at its call sites.
     */
    st = obc_mtime_read(&now);
    uptime = (st == OBC_OK) ? (uint32_t)(now & 0xFFFFFFFFu) : 0xFFFFFFFFu;

    put_u32(OBC_TLM_O_SYNC, OBC_TLM_SYNC);
    put_u32(OBC_TLM_O_SEQ, obc_tlm_seq);
    put_u32(OBC_TLM_O_UPTIME, uptime);
    put_u32(OBC_TLM_O_FRAMES, obc_frames_run);
    put_u16(OBC_TLM_O_FRAME_OVERRUNS, sat16(obc_frame_overruns));
    put_u16(OBC_TLM_O_SLACK, sat16(obc_slack_ticks_min));

    for (i = 0u; i < OBC_TLM_N_TASK_OVERRUNS; i++) {
        uint32_t v = (i < obc_task_count) ? obc_task_state[i].overruns : 0u;

        put_u8(OBC_TLM_O_TASK_OVERRUNS + i, sat8(v));
    }

    put_u16(OBC_TLM_O_REPAIRS, sat16(obc_critical_repairs));
    put_u16(OBC_TLM_O_FAILED_VOTES, sat16(obc_critical_failed_votes));
    put_u16(OBC_TLM_O_SUSPENSIONS, sat16(obc_suspension_count));
    put_u16(OBC_TLM_O_DROPS, sat16(obc_tlm_drops));
    put_u8(OBC_TLM_O_SHORT_BOOTS, sat8(obc_recover_short_boots()));

    /*
     * The mode goes through the voter, never through the mirror word. Reading
     * obc_mode directly would publish a value nothing re-derived, and a
     * corrupted mirror would put "nominal" in a frame emitted by a degraded
     * system — the one lie this telemetry exists to make impossible.
     */
    put_u8(OBC_TLM_O_MODE, obc_mode_is_safe() ? OBC_MODE_SAFE : OBC_MODE_NOMINAL);
    put_u8(OBC_TLM_O_SAFE_REASON, obc_safe_reason);
    put_u8(OBC_TLM_O_RESET_CAUSE, obc_reset_cause_previous);

    put_u8(OBC_TLM_O_SENSOR_FLAGS, obc_sensor_flags);
    for (i = 0u; i < OBC_TLM_N_SENSOR; i++) {
        put_u16(OBC_TLM_O_SENSOR + (i * OBC_TLM_W_SENSOR), obc_sensor_value[i]);
    }

    put_u16(OBC_TLM_O_SUM, frame_sum());

    obc_tlm_seq++;

    /*
     * Out on the downlink port, as raw bytes. Never on the console: a frame
     * contains NUL and every other value, and a text channel carrying it stops
     * being a text channel — which is not a cosmetic complaint. Every host-side
     * assertion that reads the console as text would go quiet, reporting the
     * subsystems it was watching as broken. The first version of this shared one
     * port and did exactly that to three unrelated tests.
     */
    {
        obc_status_t w = obc_uart_downlink_write(obc_tlm_frame, OBC_TLM_FRAME_LEN);

        if (w != OBC_OK) {
            /*
             * The frame is shed, and the fact is recorded. ADR 0009: a subsystem
             * whose purpose is observability loses its data rather than the
             * system. Spending the budget here would overrun, climb the ladder,
             * and reset a machine whose only problem is that nobody is draining
             * its downlink — and the reset would not drain it.
             *
             * The announcement goes to the console, which is the one channel
             * that does not depend on the thing that just failed. Its status is
             * discarded because there is nowhere left after it.
             */
            obc_tlm_drops++;
            obc_tlm_drops_consecutive++;
            OBC_IGNORE(obc_uart_puts("tlm    : downlink refused, frame dropped\r\n"));
            return w;
        }
        obc_tlm_drops_consecutive = 0u;
    }

    return st;
}
