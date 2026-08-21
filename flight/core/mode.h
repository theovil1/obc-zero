/*
 * Operating mode, and the degraded one in particular.
 *
 * Safe mode is defined in docs/adr/0005-safe-mode.md against the system that
 * exists — a task table, an executive, a transmit-only serial line and a record
 * that survives a reset — rather than against the command path that arrives at
 * M7. "The minimum that keeps the system contactable" cannot be implemented
 * without an uplink, and writing it that way would have constrained M7 from a
 * position of ignorance.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OBC_MODE_H
#define OBC_MODE_H

#include <stdint.h>

#include "core/status.h"

#define OBC_MODE_NOMINAL 0u
#define OBC_MODE_SAFE 1u

/*
 * Why the system went degraded. Kept distinct rather than collapsed into a
 * single "something went wrong", because M5's escalation ladder has to treat
 * a clock that stopped differently from a task that ran long.
 */
#define OBC_SAFE_NONE 0u
#define OBC_SAFE_TRAP 1u          /* a fault, restored from the record at boot */
#define OBC_SAFE_FRAME_OVERRUN 2u /* the executive missed a frame boundary */
#define OBC_SAFE_CLOCK 3u         /* mtime would not settle */

/*
 * Observable 1 of 3: a word in RAM. Answers "what is the system doing now"
 * without perturbing it, because the debugger reads it without stopping
 * anything that matters.
 */
extern volatile uint32_t obc_mode;
extern volatile uint32_t obc_safe_reason;

/*
 * Observable 3 of 3: the same fact, in a record that survives a reset.
 *
 * A separate record from the fault record on purpose. The fault record is
 * consumed and cleared at every boot, so a field inside it could not answer
 * "did a previous boot go degraded" — the question M5 needs and the one neither
 * of the other two observables can answer across a reset. Its own magic and its
 * own checksum, for the same reason the fault record has them: at cold boot
 * this is whatever the RAM held.
 */
#define OBC_MODE_MAGIC 0x0BC5AFE1u
#define OBC_MODE_CHECKSUM_SEED 0x3C3CA5A5u

typedef struct {
    volatile uint32_t magic;
    volatile uint32_t checksum;
    volatile uint32_t reason;
} obc_mode_record_t;

extern obc_mode_record_t obc_mode_record;

/*
 * Enters safe mode and says so on every channel.
 *
 * Idempotent: entering again with a different reason keeps the first, because
 * the first is the one that explains the others. Never returns to nominal —
 * exit policy needs evidence that the ground is satisfied, and there is no
 * ground before M7. That is a decision, recorded in ADR 0005, not an omission.
 */
void obc_mode_enter_safe(uint32_t reason);

/*
 * Decides the mode this boot starts in, from what the previous one left behind.
 *
 * A recorded trap means the previous boot faulted, so this one comes up
 * degraded: the trap handler resets rather than running policy, and this is
 * where the policy it deferred actually happens.
 */
void obc_mode_restore(uint32_t previous_reset_cause);

/* True if the system is degraded. Cheap enough for the dispatch loop. */
static inline int obc_mode_is_safe(void)
{
    return obc_mode == OBC_MODE_SAFE;
}

#endif /* OBC_MODE_H */
