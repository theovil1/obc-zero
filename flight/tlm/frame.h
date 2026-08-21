/*
 * Housekeeping telemetry: one fixed frame, described by one table.
 *
 * The shape was fixed in docs/adr/0008-telemetry-layout.md before this was
 * written. The decision that governs everything here is decision 1: **the field
 * definitions live in this file and nowhere else.** The host decoder does not
 * restate them, it reads obc_tlm_fields[] out of the binary under test — the
 * same way M2's conformance checker reads the task table rather than keeping its
 * own copy of the periods.
 *
 * A restated layout on the host would not fail when it drifted. It would decode
 * a frame into plausible wrong numbers, and a campaign would report them.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OBC_TLM_FRAME_H
#define OBC_TLM_FRAME_H

#include <stdint.h>

#include "core/sched.h"
#include "core/status.h"

/*
 * The sync word, and why the decoder cannot simply trust it.
 *
 * The serial line carries human-readable text as well as frames — the boot
 * banner, the safe-mode announcement — so a frame has to be findable in a stream
 * that is not all frames. This pattern is chosen to be absent from ASCII text.
 *
 * It is *not* chosen to be absent from the payload, because it cannot be: a
 * counter will eventually hold these bytes. The decoder therefore treats a sync
 * as a candidate rather than as a frame boundary, checks the trailing sum, and
 * on failure advances by one byte and looks again. Written down here because a
 * decoder that skips a whole frame on a false sync loses real frames, silently.
 */
#define OBC_TLM_SYNC 0xA5C35A3Cu

/*
 * Field kinds. The decoder needs to know how to print a value, not merely how
 * wide it is: a mode of 1 and a repair count of 1 are the same word and mean
 * unrelated things.
 */
#define OBC_TLM_KIND_SYNC 0u
#define OBC_TLM_KIND_COUNT 1u  /* monotonic, except where ADR 0008 says not */
#define OBC_TLM_KIND_ENUM 2u   /* a small set of named values */
#define OBC_TLM_KIND_BITS 3u   /* a flag word, read bit by bit */
#define OBC_TLM_KIND_SENSOR 4u /* a reading, valid only if its flag is clear */
#define OBC_TLM_KIND_SUM 5u    /* the trailing integrity word */

/*
 * A field. `count` is the number of elements, so an array is one descriptor
 * rather than N near-identical ones: eight rows differing only in an index are
 * eight chances to mistype one.
 */
typedef struct {
    const char *name;
    uint16_t offset; /* from the start of the frame */
    uint8_t width;   /* bytes per element: 1, 2 or 4 */
    uint8_t count;   /* elements; 1 for a scalar */
    uint8_t kind;
    uint8_t pad[3];
} obc_tlm_field_t;

/*
 * The offsets.
 *
 * Chained rather than written as literals, so that contiguity is a property of
 * the definition and not something to be re-checked after every edit. Inserting
 * a field moves everything after it by construction; the alternative is a column
 * of numbers where an insertion silently overlaps its neighbour.
 *
 * Task overruns are sized by OBC_MAX_TASKS rather than by the current table, so
 * that adding a task changes the frame's contents and not its layout.
 */
#define OBC_TLM_W_SYNC 4u
#define OBC_TLM_O_SYNC 0u

#define OBC_TLM_W_SEQ 4u
#define OBC_TLM_O_SEQ (OBC_TLM_O_SYNC + OBC_TLM_W_SYNC)

#define OBC_TLM_W_UPTIME 4u
#define OBC_TLM_O_UPTIME (OBC_TLM_O_SEQ + OBC_TLM_W_SEQ)

#define OBC_TLM_W_FRAMES 4u
#define OBC_TLM_O_FRAMES (OBC_TLM_O_UPTIME + OBC_TLM_W_UPTIME)

#define OBC_TLM_W_FRAME_OVERRUNS 2u
#define OBC_TLM_O_FRAME_OVERRUNS (OBC_TLM_O_FRAMES + OBC_TLM_W_FRAMES)

#define OBC_TLM_W_SLACK 2u
#define OBC_TLM_O_SLACK (OBC_TLM_O_FRAME_OVERRUNS + OBC_TLM_W_FRAME_OVERRUNS)

#define OBC_TLM_W_TASK_OVERRUNS 1u
#define OBC_TLM_N_TASK_OVERRUNS OBC_MAX_TASKS
#define OBC_TLM_O_TASK_OVERRUNS (OBC_TLM_O_SLACK + OBC_TLM_W_SLACK)

#define OBC_TLM_W_REPAIRS 2u
#define OBC_TLM_O_REPAIRS \
    (OBC_TLM_O_TASK_OVERRUNS + (OBC_TLM_W_TASK_OVERRUNS * OBC_TLM_N_TASK_OVERRUNS))

#define OBC_TLM_W_FAILED_VOTES 2u
#define OBC_TLM_O_FAILED_VOTES (OBC_TLM_O_REPAIRS + OBC_TLM_W_REPAIRS)

#define OBC_TLM_W_SUSPENSIONS 2u
#define OBC_TLM_O_SUSPENSIONS (OBC_TLM_O_FAILED_VOTES + OBC_TLM_W_FAILED_VOTES)

#define OBC_TLM_W_SHORT_BOOTS 1u
#define OBC_TLM_O_SHORT_BOOTS (OBC_TLM_O_SUSPENSIONS + OBC_TLM_W_SUSPENSIONS)

#define OBC_TLM_W_MODE 1u
#define OBC_TLM_O_MODE (OBC_TLM_O_SHORT_BOOTS + OBC_TLM_W_SHORT_BOOTS)

#define OBC_TLM_W_SAFE_REASON 1u
#define OBC_TLM_O_SAFE_REASON (OBC_TLM_O_MODE + OBC_TLM_W_MODE)

#define OBC_TLM_W_RESET_CAUSE 1u
#define OBC_TLM_O_RESET_CAUSE (OBC_TLM_O_SAFE_REASON + OBC_TLM_W_SAFE_REASON)

#define OBC_TLM_W_SENSOR_FLAGS 1u
#define OBC_TLM_O_SENSOR_FLAGS (OBC_TLM_O_RESET_CAUSE + OBC_TLM_W_RESET_CAUSE)

#define OBC_TLM_W_SENSOR 2u
#define OBC_TLM_N_SENSOR 2u
#define OBC_TLM_O_SENSOR (OBC_TLM_O_SENSOR_FLAGS + OBC_TLM_W_SENSOR_FLAGS)

#define OBC_TLM_W_SUM 2u
#define OBC_TLM_O_SUM (OBC_TLM_O_SENSOR + (OBC_TLM_W_SENSOR * OBC_TLM_N_SENSOR))

#define OBC_TLM_FRAME_LEN (OBC_TLM_O_SUM + OBC_TLM_W_SUM)

/*
 * The frame buffer, in RAM, exported so the debugger can read the last frame
 * emitted without having to reassemble it from the serial capture. A test that
 * decodes only the serial stream cannot tell a packing error from a UART that
 * dropped a byte, and those need different fixes.
 */
extern volatile uint8_t obc_tlm_frame[OBC_TLM_FRAME_LEN];

/*
 * Kept against --gc-sections.
 *
 * Nothing in flight code reads these: the sync word is used through its macro,
 * and the compiler folds the sensor bounds into the comparisons that use them.
 * The linker is right to drop unreferenced constants, and the result is wrong
 * here — **these symbols are in the image for the ground, not for the vehicle.**
 * They are the layout the host decodes against, and a build that discards them
 * produces frames nobody can read.
 *
 * `retain` rather than `used`: `used` keeps the symbol in its object file, which
 * --gc-sections then discards anyway.
 */
#define OBC_TLM_KEEP __attribute__((used, retain))

/*
 * The layout, read by the host out of the ELF.
 *
 * The sync word is here as a symbol and not only as a macro, because a macro is
 * not reliably in a binary's debug info and the host would then have to restate
 * it — which is the one thing this whole arrangement exists to avoid. A sync
 * word is layout, and layout comes from the vehicle.
 */
extern const obc_tlm_field_t obc_tlm_fields[];
extern const uint32_t obc_tlm_field_count;
extern const uint32_t obc_tlm_frame_len;
extern const uint32_t obc_tlm_sync;

/*
 * The sequence number, and the one thing about it that is not obvious.
 *
 * **It is not monotonic across a rung-2 reset**, by choice. ADR 0008 decision 3
 * makes the reset observable by returning this to zero, because a subsystem
 * reset whose effect cannot be seen from outside is a rung that does nothing. A
 * decoder that assumes monotonicity will fail loudly here, which is the intent:
 * an extra "resets" field would let the same decoder ignore it silently.
 */
extern volatile uint32_t obc_tlm_seq;

/* Rung-2 resets performed, kept for the report rather than for the frame. */
extern volatile uint32_t obc_tlm_subsystem_resets;

/*
 * Audits the descriptor table against the frame it claims to describe: fields
 * strictly increasing, contiguous, no overlap, ending exactly at the frame
 * length.
 *
 * The offsets above are chained, so contiguity is true by construction — but a
 * table row can still name the wrong macro, and that is the failure this
 * catches. Run once at init, bounded by the table, and it **refuses**: a system
 * that emits frames the host cannot decode is worse than one that says so.
 */
OBC_MUST_CHECK obc_status_t obc_tlm_init(void);

/*
 * Packs the current housekeeping values into obc_tlm_frame and writes it to the
 * UART. Explicit byte writes at descriptor offsets, never a struct cast: a
 * layout that depends on compiler padding changes with a compiler flag, and the
 * size reference would report the drift without saying what caused it.
 *
 * Returns the UART's status. A dropped byte is a real event on this line and the
 * caller is not allowed to assume it did not happen.
 */
OBC_MUST_CHECK obc_status_t obc_tlm_emit(void);

/*
 * Rung 2: returns the subsystem's own state to a known value.
 *
 * Sequence to zero, frame buffer cleared, sensor history dropped. Deliberately
 * *not* the counters that describe the system's history — a recovery action that
 * erases the evidence of why it was needed leaves a campaign with nothing to
 * read.
 */
void obc_tlm_subsystem_reset(void);

#endif /* OBC_TLM_FRAME_H */
