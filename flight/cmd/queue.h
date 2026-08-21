/*
 * The time-tagged command queue.
 *
 * Statically sized, in .bss, and it refuses when it is full rather than
 * overwriting. That is the M7 criterion, and it is also ADR 0011 decision 1
 * applied to storage: a queue that drops its oldest entry to make room lets the
 * outside decide which commands the vehicle forgets. Refusing means the ground
 * knows what did not get in.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OBC_CMD_QUEUE_H
#define OBC_CMD_QUEUE_H

#include <stdint.h>

#include "core/sched.h"
#include "core/status.h"

/*
 * Depth. Small on purpose: the queue holds commands waiting for their tick, not
 * a backlog. There is no mission, so no mission parameter fixes it.
 *
 * **Four rather than eight, and the reason is that eight cannot be shown to
 * refuse.** The uplink carries six to twelve frames per run — its throughput is
 * set by the receive FIFO and the poll period, not by the baud rate — so filling
 * an eight-deep queue lands exactly on that limit and the full-queue rejection
 * fired or did not depending on how busy the host was. A depth whose refusal
 * cannot be demonstrated is a depth whose refusal is a claim.
 *
 * Declared arbitrary in the same breath: four is not derived from a mission
 * either. What is derived is that it is small enough to be exercised.
 */
#define OBC_CMD_QUEUE_LEN 4u

/* Read by the host, so a test can send one more frame than the queue holds
 * without restating how many that is. */
#define OBC_CMD_QUEUE_KEEP __attribute__((used, retain))
extern const uint32_t obc_cmd_queue_len;

typedef struct {
    uint32_t when;    /* machine-timer ticks; 0 means the next frame */
    uint32_t arg;
    uint32_t counter; /* the uplink counter it arrived with, for the report */
    uint8_t opcode;
    uint8_t occupied;
    uint8_t pad[2];
} obc_cmd_slot_t;

extern obc_cmd_slot_t obc_cmd_queue[OBC_CMD_QUEUE_LEN];
extern volatile uint32_t obc_cmd_queued;   /* entries currently held */
extern volatile uint32_t obc_cmd_executed; /* entries run, saturating */
extern volatile uint32_t obc_cmd_late;     /* run more than one tick past due */

/*
 * How late a command may run and still count as on time.
 *
 * One frame's worth of ticks, because the executive can only act at a frame
 * boundary: a command tagged for the middle of a frame is on time if it runs in
 * that frame. Measuring against the exact tick would count every command late
 * and the M7 criterion would be unmeetable by construction rather than by
 * defect.
 */
#define OBC_CMD_LATE_TICKS OBC_FRAME_TICKS

/*
 * The arming state for critical commands.
 *
 * A critical command is refused unless the immediately preceding accepted
 * command armed that exact opcode, and arming expires. Two frames rather than
 * one is the whole mechanism: a single corrupted opcode cannot reach a critical
 * handler, because it would also have to be preceded by an arm naming it.
 *
 * The expiry is in frames rather than in accepted commands, so that an arm
 * cannot sit indefinitely waiting for the frame that uses it. Arbitrary, and
 * declared so.
 */
#define OBC_CMD_ARM_FRAMES 4u
#define OBC_CMD_ARM_NONE 0xFFFFu

extern volatile uint32_t obc_cmd_armed_opcode; /* OBC_CMD_ARM_NONE when not armed */
extern volatile uint32_t obc_cmd_armed_until;  /* frame index the arm expires at */

/* Empties the queue and drops any arming. Called at init and by nothing else:
 * this is not a recovery action, and ADR 0011 keeps the command path out of the
 * escalation ladder. */
void obc_cmd_queue_init(void);

/*
 * Places a validated command. Returns OBC_ERR_INVALID if the queue is full —
 * never overwrites, never drops the oldest.
 */
OBC_MUST_CHECK obc_status_t obc_cmd_queue_put(uint8_t opcode, uint32_t when,
                                              uint32_t arg, uint32_t counter);

/*
 * Runs every command whose tick has arrived, oldest first.
 *
 * Bounded by OBC_CMD_QUEUE_LEN twice over: the outer pass runs at most that many
 * commands, and the inner search for the oldest is over the same array. `now` is
 * passed in rather than read here, so the executive reads the clock once per
 * frame through the one call site that degrades on an unstable timer.
 */
void obc_cmd_queue_run(uint32_t now, uint32_t frame);

#endif /* OBC_CMD_QUEUE_H */
