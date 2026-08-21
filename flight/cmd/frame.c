/*
 * Command ingest: validate, and refuse in a way somebody can read.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cmd/frame.h"

#include <stddef.h>
#include <stdint.h>

#include "cmd/queue.h"
#include "core/status.h"
#include "hal/uart.h"

OBC_CMD_KEEP const obc_cmd_field_t obc_cmd_fields[] = {
    { "sync", OBC_CMD_O_SYNC, OBC_CMD_W_SYNC, 0 },
    { "counter", OBC_CMD_O_COUNTER, OBC_CMD_W_COUNTER, 0 },
    { "length", OBC_CMD_O_LENGTH, OBC_CMD_W_LENGTH, 0 },
    { "opcode", OBC_CMD_O_OPCODE, OBC_CMD_W_OPCODE, 0 },
    { "when", OBC_CMD_O_WHEN, OBC_CMD_W_WHEN, 0 },
    { "arg", OBC_CMD_O_ARG, OBC_CMD_W_ARG, 0 },
    { "sum", OBC_CMD_O_SUM, OBC_CMD_W_SUM, 0 },
};

OBC_CMD_KEEP const uint32_t obc_cmd_field_count =
    (uint32_t)(sizeof(obc_cmd_fields) / sizeof(obc_cmd_fields[0]));

OBC_CMD_KEEP const uint32_t obc_cmd_frame_len = OBC_CMD_FRAME_LEN;
OBC_CMD_KEEP const uint32_t obc_cmd_sync = OBC_CMD_SYNC;
OBC_CMD_KEEP const uint32_t obc_cmd_reject_count = OBC_CMD_REJECT_COUNT;

volatile uint32_t obc_cmd_examined;
volatile uint32_t obc_cmd_accepted;
volatile uint32_t obc_cmd_rejected[OBC_CMD_REJECT_COUNT];
volatile uint32_t obc_cmd_announced;

/* The last accepted uplink counter. A frame must exceed it or it is a replay. */
static uint32_t s_last_counter;

/* Set only by an audit that passed. A table that failed it must not be used to
 * execute anything, and declining to ingest is something the vehicle can do
 * about a defect it cannot repair. */
static uint32_t s_table_ok;

static const char *const s_reason_name[OBC_CMD_REJECT_COUNT] = {
    "sync", "length", "sum", "opcode", "arg", "replay", "queue", "not-armed",
};

/*
 * Saturating, because these are counters the outside drives.
 *
 * ADR 0012 decision 1: a counter that has gone round reports a sustained flood
 * as a handful of rejections, which is a large problem shown as a small one and
 * worse than showing nothing. At the ceiling the value is still true — it means
 * "at least this many".
 */
static void bump(volatile uint32_t *counter)
{
    if (*counter < OBC_CMD_COUNTER_MAX) {
        (*counter)++;
    }
}

static uint32_t get_u8(const volatile uint8_t *frame, uint32_t offset)
{
    return frame[offset];
}

static uint32_t get_u16(const volatile uint8_t *frame, uint32_t offset)
{
    return (uint32_t)frame[offset] | ((uint32_t)frame[offset + 1u] << 8);
}

static uint32_t get_u32(const volatile uint8_t *frame, uint32_t offset)
{
    return (uint32_t)frame[offset] | ((uint32_t)frame[offset + 1u] << 8)
           | ((uint32_t)frame[offset + 2u] << 16)
           | ((uint32_t)frame[offset + 3u] << 24);
}

/*
 * Not a CRC, and the same one the downlink uses. It catches a corrupted or
 * truncated frame, which is the threat on a link that drops bytes; it makes no
 * claim about a frame built to defeat it, and ADR 0011 decision 4 says plainly
 * that a frame passing this is well formed and not thereby authorised.
 */
uint32_t obc_cmd_sum(const volatile uint8_t *frame, uint32_t upto)
{
    uint32_t sum = 0u;
    uint32_t i;

    for (i = 0u; i < upto; i++) {
        sum += frame[i];
        sum = (sum + (sum >> 16)) & 0xFFFFu;
    }
    return sum ^ 0xFFFFu;
}

/*
 * One console line per reason, ever — not per frame.
 *
 * A hundred thousand frames of announcement is a denial of service against the
 * log, and against the campaign's own readability. Bounding the output by the
 * number of reasons rather than by the number of frames is what survives someone
 * transmitting garbage for an hour. ADR 0011 decision 3.
 */
static void announce_once(uint32_t reason)
{
    if ((obc_cmd_announced & (1u << reason)) != 0u) {
        return;
    }
    obc_cmd_announced |= (1u << reason);
    OBC_IGNORE(obc_uart_puts("cmd    : rejected, first "));
    OBC_IGNORE(obc_uart_puts(s_reason_name[reason]));
    OBC_IGNORE(obc_uart_puts("\r\n"));
}

static obc_status_t refuse(uint32_t reason, uint32_t *out)
{
    bump(&obc_cmd_rejected[reason]);
    announce_once(reason);
    if (out != NULL) {
        *out = reason;
    }
    return OBC_ERR_INVALID;
}

/* The partial frame being assembled. Not .noinit: a reset must not resume
 * halfway through a command somebody sent to a previous boot. */
static volatile uint8_t s_rx[OBC_CMD_FRAME_LEN];
static uint32_t s_fill;

void obc_cmd_poll(void)
{
    uint32_t budget;
    uint32_t taken = 0u;

    if (s_table_ok == 0u) {
        return;
    }

    for (budget = 0u; budget < OBC_CMD_RX_BYTES; budget++) {
        uint8_t byte = 0u;

        if (obc_uart_uplink_getc(&byte) != OBC_OK) {
            break;
        }
        taken++;
        s_rx[s_fill] = byte;
        s_fill++;
        if (s_fill == OBC_CMD_FRAME_LEN) {
            OBC_IGNORE(obc_cmd_ingest(s_rx, OBC_CMD_FRAME_LEN, NULL));
            s_fill = 0u;
        }
    }

    /*
     * Nothing waiting and a partial frame held: it was truncated. Counted as
     * examined and refused as a length error, because ADR 0013's identity has no
     * column for a frame that was neither — and a truncated command silently
     * dropped is indistinguishable from one that never arrived.
     */
    if (taken == 0u && s_fill != 0u) {
        s_fill = 0u;
        bump(&obc_cmd_examined);
        OBC_IGNORE(refuse(OBC_CMD_REJECT_LENGTH, NULL));
    }
}

obc_status_t obc_cmd_init(void)
{
    uint32_t i;
    uint32_t j;

    if (obc_cmd_count == 0u) {
        return OBC_ERR_INVALID;
    }

    /*
     * The descriptor table must describe the frame the parser reads: contiguous,
     * in order, ending exactly at the frame length. Same audit as the telemetry
     * layout, and it refuses for the same reason — a table that does not match
     * makes the ground build frames the vehicle cannot parse, and every
     * rejection would name the wrong thing.
     */
    {
        uint32_t expected = 0u;

        for (i = 0u; i < obc_cmd_field_count; i++) {
            if (obc_cmd_fields[i].offset != expected) {
                return OBC_ERR_INVALID;
            }
            expected += obc_cmd_fields[i].width;
        }
        if (expected != OBC_CMD_FRAME_LEN) {
            return OBC_ERR_INVALID;
        }
    }

    for (i = 0u; i < obc_cmd_count; i++) {
        if (obc_cmd_table[i].fn == 0) {
            return OBC_ERR_INVALID; /* an opcode nothing implements */
        }
        if (obc_cmd_table[i].arg_min > obc_cmd_table[i].arg_max) {
            return OBC_ERR_INVALID; /* a range that admits nothing */
        }
        for (j = 0u; j < i; j++) {
            if (obc_cmd_table[j].opcode == obc_cmd_table[i].opcode) {
                return OBC_ERR_INVALID; /* the second row would never be reached */
            }
        }
    }

    obc_cmd_queue_init();
    obc_cmd_examined = 0u;
    obc_cmd_accepted = 0u;
    s_fill = 0u;
    obc_cmd_announced = 0u;
    s_last_counter = 0u;
    for (i = 0u; i < OBC_CMD_REJECT_COUNT; i++) {
        obc_cmd_rejected[i] = 0u;
    }
    s_table_ok = 1u;
    return OBC_OK;
}

static const obc_cmd_t *lookup(uint8_t opcode)
{
    uint32_t i;

    for (i = 0u; i < obc_cmd_count; i++) {
        if (obc_cmd_table[i].opcode == opcode) {
            return &obc_cmd_table[i];
        }
    }
    return NULL;
}

obc_status_t obc_cmd_ingest(const volatile uint8_t *frame, uint32_t len,
                            uint32_t *reason)
{
    const obc_cmd_t *cmd;
    uint32_t counter;
    uint32_t arg;
    uint32_t when;
    uint8_t opcode;

    if (frame == NULL || s_table_ok == 0u) {
        return OBC_ERR_INVALID;
    }

    /*
     * Counted first, before any check can refuse.
     *
     * This is the denominator ADR 0013 obligation 2 needs. A frame examined and
     * counted in neither column below was silently discarded — and silent
     * discard is precisely what a passing fuzz campaign looks like from outside.
     * Every path from here reaches exactly one of accept or refuse.
     */
    bump(&obc_cmd_examined);

    if (len != OBC_CMD_FRAME_LEN) {
        return refuse(OBC_CMD_REJECT_LENGTH, reason);
    }
    if (get_u32(frame, OBC_CMD_O_SYNC) != OBC_CMD_SYNC) {
        return refuse(OBC_CMD_REJECT_SYNC, reason);
    }
    if (get_u8(frame, OBC_CMD_O_LENGTH) != OBC_CMD_FRAME_LEN) {
        return refuse(OBC_CMD_REJECT_LENGTH, reason);
    }
    if (get_u16(frame, OBC_CMD_O_SUM) != obc_cmd_sum(frame, OBC_CMD_O_SUM)) {
        return refuse(OBC_CMD_REJECT_SUM, reason);
    }

    opcode = (uint8_t)get_u8(frame, OBC_CMD_O_OPCODE);
    cmd = lookup(opcode);
    if (cmd == NULL) {
        return refuse(OBC_CMD_REJECT_OPCODE, reason);
    }

    /*
     * The argument's range lives in the table; the decision about *this*
     * argument is made here, every time. ADR 0012 decision 2 — a static
     * assertion in this position would pass at the desk and let the vehicle
     * execute whatever number arrived.
     */
    arg = get_u32(frame, OBC_CMD_O_ARG);
    if (arg < cmd->arg_min || arg > cmd->arg_max) {
        return refuse(OBC_CMD_REJECT_ARG, reason);
    }

    counter = get_u32(frame, OBC_CMD_O_COUNTER);
    if (counter <= s_last_counter) {
        return refuse(OBC_CMD_REJECT_REPLAY, reason);
    }

    if (cmd->critical != 0u && obc_cmd_armed_opcode != opcode) {
        return refuse(OBC_CMD_REJECT_NOT_ARMED, reason);
    }

    when = get_u32(frame, OBC_CMD_O_WHEN);
    if (obc_cmd_queue_put(opcode, when, arg, counter) != OBC_OK) {
        return refuse(OBC_CMD_REJECT_QUEUE, reason);
    }

    /*
     * Accepted, and only now does the counter advance. Advancing it earlier
     * would let a frame that is rejected further down still consume a counter
     * value, so a ground station retrying a refused command with the same
     * counter would be told it is a replay — a rejection reason that names the
     * wrong thing.
     */
    s_last_counter = counter;
    bump(&obc_cmd_accepted);
    if (reason != NULL) {
        *reason = OBC_CMD_REJECT_COUNT; /* no reason: it was accepted */
    }
    return OBC_OK;
}
