/*
 * The time-tagged command queue.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cmd/queue.h"

#include <stddef.h>
#include <stdint.h>

#include "cmd/frame.h"
#include "core/status.h"

obc_cmd_slot_t obc_cmd_queue[OBC_CMD_QUEUE_LEN];
OBC_CMD_QUEUE_KEEP const uint32_t obc_cmd_queue_len = OBC_CMD_QUEUE_LEN;
volatile uint32_t obc_cmd_queued;
volatile uint32_t obc_cmd_executed;
volatile uint32_t obc_cmd_late;

volatile uint32_t obc_cmd_armed_opcode;
volatile uint32_t obc_cmd_armed_until;

/* Set by the arm handler, consumed here. The handler cannot reach into this
 * state directly: a command handler that armed itself would make arming a
 * property of the handler rather than of the sequence. */
extern volatile uint32_t obc_cmd_arm_target;

void obc_cmd_queue_init(void)
{
    uint32_t i;

    for (i = 0u; i < OBC_CMD_QUEUE_LEN; i++) {
        obc_cmd_queue[i].occupied = 0u;
        obc_cmd_queue[i].when = 0u;
        obc_cmd_queue[i].arg = 0u;
        obc_cmd_queue[i].counter = 0u;
        obc_cmd_queue[i].opcode = 0u;
    }
    obc_cmd_queued = 0u;
    obc_cmd_armed_opcode = OBC_CMD_ARM_NONE;
    obc_cmd_armed_until = 0u;
}

obc_status_t obc_cmd_queue_put(uint8_t opcode, uint32_t when, uint32_t arg,
                               uint32_t counter)
{
    uint32_t i;

    for (i = 0u; i < OBC_CMD_QUEUE_LEN; i++) {
        if (obc_cmd_queue[i].occupied != 0u) {
            continue;
        }
        obc_cmd_queue[i].opcode = opcode;
        obc_cmd_queue[i].when = when;
        obc_cmd_queue[i].arg = arg;
        obc_cmd_queue[i].counter = counter;
        obc_cmd_queue[i].occupied = 1u;
        obc_cmd_queued++;
        return OBC_OK;
    }

    /*
     * Full. Refused, never overwritten and never at the expense of the oldest
     * entry: a queue that evicts to make room lets whoever is transmitting
     * decide which commands the vehicle forgets, and the ground has no way to
     * know which. Refusing means the ground is told.
     */
    return OBC_ERR_INVALID;
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

void obc_cmd_queue_run(uint32_t now, uint32_t frame)
{
    uint32_t pass;

    /*
     * Arming expires on a frame count, so an arm cannot sit indefinitely waiting
     * for the frame that uses it. Checked before anything runs, so a critical
     * command queued while armed still meets an expired arm if its tick has
     * moved past the window.
     */
    if (obc_cmd_armed_opcode != OBC_CMD_ARM_NONE && frame >= obc_cmd_armed_until) {
        obc_cmd_armed_opcode = OBC_CMD_ARM_NONE;
    }

    /*
     * At most one pass per slot, so the whole thing is bounded twice: the outer
     * loop cannot run more commands than the queue holds, and the inner search
     * is over the same fixed array. No recursion, no unbounded wait.
     */
    for (pass = 0u; pass < OBC_CMD_QUEUE_LEN; pass++) {
        uint32_t oldest = OBC_CMD_QUEUE_LEN;
        uint32_t i;
        const obc_cmd_t *cmd;

        for (i = 0u; i < OBC_CMD_QUEUE_LEN; i++) {
            if (obc_cmd_queue[i].occupied == 0u) {
                continue;
            }
            /* Not yet due. Unsigned comparison against a wrapping clock is
             * wrong at the wrap and right everywhere else; `when` is an absolute
             * tick supplied by the ground, and a ground that sends a tag across
             * a wrap gets it late rather than never. Recorded rather than
             * papered over. */
            if (obc_cmd_queue[i].when > now) {
                continue;
            }
            if (oldest == OBC_CMD_QUEUE_LEN
                || obc_cmd_queue[i].when < obc_cmd_queue[oldest].when) {
                oldest = i;
            }
        }

        if (oldest == OBC_CMD_QUEUE_LEN) {
            return; /* nothing due */
        }

        cmd = lookup(obc_cmd_queue[oldest].opcode);
        obc_cmd_queue[oldest].occupied = 0u;
        if (obc_cmd_queued > 0u) {
            obc_cmd_queued--;
        }

        if (cmd == NULL) {
            continue; /* the table changed under a queued entry; drop it */
        }

        /*
         * Late is measured against the frame, not against the tick the ground
         * asked for: the executive can only act at a frame boundary, so a
         * command due mid-frame is on time if it runs in that frame. The M7
         * criterion is one scheduler tick and this is what makes it checkable.
         */
        if (now > obc_cmd_queue[oldest].when + OBC_CMD_LATE_TICKS) {
            if (obc_cmd_late < OBC_CMD_COUNTER_MAX) {
                obc_cmd_late++;
            }
        }

        cmd->fn(obc_cmd_queue[oldest].arg);
        if (obc_cmd_executed < OBC_CMD_COUNTER_MAX) {
            obc_cmd_executed++;
        }

        /*
         * The arm handler records its target; the arming itself is established
         * here, after the handler ran. Keeping it out of the handler means
         * arming is a property of the accepted sequence rather than of what a
         * handler chose to do to global state.
         */
        if (obc_cmd_arm_target != 0u) {
            obc_cmd_armed_opcode = obc_cmd_arm_target;
            obc_cmd_armed_until = frame + OBC_CMD_ARM_FRAMES;
            obc_cmd_arm_target = 0u;
        }
    }
}
