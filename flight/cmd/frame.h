/*
 * Command frames: one fixed layout, one closed set of reasons to refuse them.
 *
 * The policy was settled before any of this was written:
 *
 *   ADR 0011  an input is not a fault. Surviving a malformed frame is refusing
 *             it and changing nothing else, and the command path therefore never
 *             enters the escalation ladder.
 *   ADR 0012  counters the outside drives saturate; an argument's range is a
 *             build property, rejecting a particular argument is a flight
 *             decision; a campaign runs on the image that flies.
 *   ADR 0013  a green fuzz campaign has to prove it reached the parser.
 *
 * The layout follows M6's: a descriptor table the host reads out of the ELF, so
 * the ground and the vehicle cannot hold different opinions about where a field
 * is. A wire format is the last place a second list belongs.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OBC_CMD_FRAME_H
#define OBC_CMD_FRAME_H

#include <stdint.h>

#include "core/status.h"

/*
 * The uplink's sync word, and it is deliberately not the downlink's.
 *
 * Two directions on one physical link, and a decoder that accepted either
 * pattern in either direction would happily parse the vehicle's own telemetry
 * as a command the moment anything looped back. That is not a hypothetical on a
 * bench with a shorted connector.
 */
#define OBC_CMD_SYNC 0x5A3CA5C3u

/* The closed set of rejection reasons.
 *
 * Closed, and fixed at compile time, because ADR 0012 requires that nothing
 * arriving from outside changes how many counters exist or how many fields a
 * frame has. Adding a reason is a layout change the ground picks up from the
 * binary; it is never something an input can cause. */
#define OBC_CMD_REJECT_SYNC 0u      /* the sync word did not match */
#define OBC_CMD_REJECT_LENGTH 1u    /* declared length is not the frame's */
#define OBC_CMD_REJECT_SUM 2u       /* the integrity word did not check */
#define OBC_CMD_REJECT_OPCODE 3u    /* no such command */
#define OBC_CMD_REJECT_ARG 4u       /* the argument is outside its range */
#define OBC_CMD_REJECT_REPLAY 5u    /* the counter did not advance */
#define OBC_CMD_REJECT_QUEUE 6u     /* the queue is full */
#define OBC_CMD_REJECT_NOT_ARMED 7u /* a critical command with no arming */
#define OBC_CMD_REJECT_COUNT 8u

/*
 * Frame offsets, chained so contiguity is a property of the definition rather
 * than of somebody's attention. Same construction as the telemetry frame, and
 * the same run-time audit refuses a table that does not describe it.
 */
#define OBC_CMD_W_SYNC 4u
#define OBC_CMD_O_SYNC 0u

/*
 * The uplink counter, and it is what makes a replay a rejection rather than an
 * execution.
 *
 * Strictly increasing. A frame whose counter does not exceed the last accepted
 * one is refused as a replay — which covers a recording played back, and also a
 * frame duplicated by a link that retransmits. It is not a security mechanism
 * and does not pretend to be: anyone who can forge a frame can forge a counter.
 * ADR 0011 decision 4 says where authentication is not.
 */
#define OBC_CMD_W_COUNTER 4u
#define OBC_CMD_O_COUNTER (OBC_CMD_O_SYNC + OBC_CMD_W_SYNC)

/* Declared length, checked against the frame the vehicle actually reads. A
 * length field that nothing verifies is the classic way a parser is persuaded to
 * read past its buffer. */
#define OBC_CMD_W_LENGTH 1u
#define OBC_CMD_O_LENGTH (OBC_CMD_O_COUNTER + OBC_CMD_W_COUNTER)

#define OBC_CMD_W_OPCODE 1u
#define OBC_CMD_O_OPCODE (OBC_CMD_O_LENGTH + OBC_CMD_W_LENGTH)

/* When to run it, in machine-timer ticks. Zero means the next frame. */
#define OBC_CMD_W_WHEN 4u
#define OBC_CMD_O_WHEN (OBC_CMD_O_OPCODE + OBC_CMD_W_OPCODE)

#define OBC_CMD_W_ARG 4u
#define OBC_CMD_O_ARG (OBC_CMD_O_WHEN + OBC_CMD_W_WHEN)

#define OBC_CMD_W_SUM 2u
#define OBC_CMD_O_SUM (OBC_CMD_O_ARG + OBC_CMD_W_ARG)

#define OBC_CMD_FRAME_LEN (OBC_CMD_O_SUM + OBC_CMD_W_SUM)

/*
 * A command, as declared. Entirely const and in flash, like the task table: a
 * corrupted RAM word must not be able to widen an argument range or redirect a
 * handler.
 */
typedef void (*obc_cmd_fn)(uint32_t arg);

typedef struct {
    const char *name;
    uint8_t opcode;
    uint8_t critical; /* needs an arm-then-execute sequence */
    uint8_t pad[2];
    uint32_t arg_min; /* inclusive */
    uint32_t arg_max; /* inclusive */
    obc_cmd_fn fn;
} obc_cmd_t;

extern const obc_cmd_t obc_cmd_table[];
extern const uint32_t obc_cmd_count;

/* Read by the host out of the ELF, never restated. Retained against
 * --gc-sections for the same reason the telemetry descriptors are: they are in
 * the image for the ground. */
#define OBC_CMD_KEEP __attribute__((used, retain))

/*
 * The uplink layout, read by the host out of the ELF.
 *
 * The same construction as the telemetry descriptors and for the same reason: a
 * harness that built frames from its own copy of these offsets would keep
 * building valid frames after the flight layout moved, and every rejection it
 * then saw would name the wrong thing.
 *
 * It matters more here than on the downlink. A decoder with a stale layout
 * misreads a report; a *fuzzer* with a stale layout is testing a format the
 * vehicle does not speak, and would report a hundred thousand clean rejections
 * as evidence that the parser works.
 */
typedef struct {
    const char *name;
    uint16_t offset;
    uint8_t width;
    uint8_t pad;
} obc_cmd_field_t;

extern const obc_cmd_field_t obc_cmd_fields[];
extern const uint32_t obc_cmd_field_count;
extern const uint32_t obc_cmd_frame_len;
extern const uint32_t obc_cmd_sync;
extern const uint32_t obc_cmd_reject_count;

/*
 * Counters. Saturating, per ADR 0012 decision 1: these are memory the outside
 * writes, and a counter that has gone round reports a sustained flood as a
 * handful of rejections — a large problem shown as a small one.
 *
 * `examined` is the denominator the fuzz campaign needs. ADR 0013 obligation 2
 * requires
 *
 *     examined == accepted + sum(rejected)
 *
 * and a frame in neither column was silently discarded, which is exactly what a
 * passing campaign looks like from outside.
 */
#define OBC_CMD_COUNTER_MAX 0xFFFFFFFFu

extern volatile uint32_t obc_cmd_examined;
extern volatile uint32_t obc_cmd_accepted;
extern volatile uint32_t obc_cmd_rejected[OBC_CMD_REJECT_COUNT];

/* Which reasons have already announced themselves. One console line per reason,
 * not per frame: at 100 000 frames the second is a denial of service against the
 * log. ADR 0011 decision 3. */
extern volatile uint32_t obc_cmd_announced;

/*
 * Audits the command table against what the parser assumes, and refuses.
 *
 * Build-time properties only — ADR 0012 decision 2. Every opcode has a handler,
 * no opcode is declared twice, and every range has min <= max. **A vehicle
 * cannot repair a table it shipped with**, so this belongs on the ground and is
 * checked there too, by `make cmd-table-check`, against the binary. It is here
 * as well because a table that fails it must not be used to execute anything,
 * and refusing to ingest is something the vehicle *can* do.
 *
 * What is deliberately NOT here: whether a particular argument is in range. That
 * depends on what arrived and is decided per frame, in obc_cmd_ingest.
 */
OBC_MUST_CHECK obc_status_t obc_cmd_init(void);

/*
 * Validates one frame and queues it, or refuses it and says why.
 *
 * Returns OBC_OK if the frame was accepted, OBC_ERR_INVALID otherwise, and
 * *reason carries which of the closed set applied. Never escalates: a malformed
 * frame is an input, not a fault, and a vehicle anybody can reset by
 * transmitting garbage is a design defect rather than a validation weakness.
 */
OBC_MUST_CHECK obc_status_t obc_cmd_ingest(const volatile uint8_t *frame,
                                           uint32_t len, uint32_t *reason);

/*
 * How many uplink bytes one dispatch may take.
 *
 * Bounded, and the bound is the task's budget rather than a round number — the
 * lesson ADR 0009 paid for on the transmit side. Eight frames' worth: enough
 * that a campaign is not throttled by the ingest path, small enough that the
 * dispatch stays inside its budget with the margin asserted in tasks.c.
 */
#define OBC_CMD_RX_BYTES (8u * OBC_CMD_FRAME_LEN)

/*
 * Takes what the uplink has and ingests whole frames.
 *
 * **Fixed-size framing, resynchronised by silence.** The vehicle counts
 * OBC_CMD_FRAME_LEN bytes and examines them; a dispatch that finds nothing
 * waiting while holding a partial frame treats it as truncated — counted,
 * rejected as a length error, and the buffer realigned.
 *
 * Not a byte-by-byte sync search, and the reason is the counting rather than the
 * cost. A shift-by-one resync examines a new window per byte, so a stream of
 * garbage would inflate `examined` far past the number of frames anyone sent,
 * and ADR 0013 obligation 1 — the two counts agree, one at each end — would stop
 * meaning anything. Here one frame sent is one frame examined, exactly.
 *
 * The limit that buys: a link that inserts or drops a single byte stays
 * misaligned until the ground pauses for a dispatch. Named rather than hidden;
 * on a real radio the answer is a preamble, which is a HAL concern and not this
 * one.
 */
void obc_cmd_poll(void);

/* The integrity word the vehicle computes. Exposed so the harness can build a
 * well-formed frame without reimplementing it — the *accept* path has to be
 * exercised too, and a harness that cannot produce a valid frame cannot exercise
 * it. Rejection is still checked against an independent implementation. */
uint32_t obc_cmd_sum(const volatile uint8_t *frame, uint32_t upto);

#endif /* OBC_CMD_FRAME_H */
