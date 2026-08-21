/*
 * Triple-redundant critical state, read through a voter.
 *
 * What qualifies as critical is decided by the criterion in
 * docs/adr/0006-what-is-critical.md, applied before this file was written so
 * that "critical" could not come to mean "whatever fit". Today it admits one
 * live variable: the operating mode.
 *
 * The rule this implements is that critical state is never read directly. A
 * caller that reads a copy is reading one opinion; the point of three copies is
 * that the disagreement between them is the signal.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OBC_CRITICAL_H
#define OBC_CRITICAL_H

#include <stdint.h>

#include "core/status.h"

#define OBC_CRITICAL_COPIES 3u

/*
 * One copy. The checksum is per copy rather than one over all three, and that
 * is the difference between detecting corruption and locating it.
 *
 * With a majority vote alone, three copies holding three different values give
 * no answer at all. With a checksum each, the copies that survived their own
 * integrity check can be told from the ones that did not, and a vote among the
 * survivors is still meaningful.
 */
typedef struct {
    volatile uint32_t value;
    volatile uint32_t checksum;
} obc_critical_copy_t;

/*
 * Per-copy checksum seeds. Each copy uses a different one, so that a corruption
 * writing the same pattern over two copies — a burst, a stuck DMA, a wild
 * pointer walking forward — cannot leave both self-consistent. A single shared
 * seed would make identical corruption look like agreement.
 */
#define OBC_CRITICAL_SEED_0 0xA5A50001u
#define OBC_CRITICAL_SEED_1 0x5A5A0002u
#define OBC_CRITICAL_SEED_2 0xC3C30003u

/*
 * One protected item: three copies of one value, one copy in each region.
 *
 * **Three regions, not three copies of a variable.** The distinction decides
 * what the next critical state costs. The 320-byte guard buys separation
 * between the *regions*, once; a second item places its three copies in the
 * same three sections and rides on the separation already paid for, at a
 * marginal cost of its own twenty-four bytes and nothing else.
 *
 * Had this been built as one variable in three places, every future item would
 * have repaid the separation — 320 bytes each, against a 3072-byte line, which
 * runs out at the ninth item.
 *
 * The descriptor is const and lives in flash, so a corrupted RAM word cannot
 * redirect the voter at the copies it is meant to be checking.
 */
typedef struct {
    const char *name;
    obc_critical_copy_t *const copies[OBC_CRITICAL_COPIES];
    uint32_t seeds[OBC_CRITICAL_COPIES];
} obc_critical_item_t;

/* The registry, walked by the scrubber. Const, in flash, compile-time sized. */
extern const obc_critical_item_t *const obc_critical_items[];
extern const uint32_t obc_critical_item_count;

/* The one item the criterion in ADR 0006 admits today: the operating mode. */
extern const obc_critical_item_t obc_critical_mode;

/*
 * Repairs performed, and votes that could not be resolved.
 *
 * **`failed_votes` counts attempts, not events**, and the name now says so.
 * Measured over the M4 campaign: one irreparable corruption produces exactly 66
 * of them in a sixteen-frame window, because every subsequent read votes again
 * and fails again. Over a long soak the same single corruption reads as
 * millions.
 *
 * That is the right thing to count for "is the voter being asked and failing",
 * and the wrong thing for "how many times did the system enter an unresolvable
 * state" — which is what an escalation ladder needs. The second counter is M5's
 * to add; this commit only stops the first one claiming to be it.
 */
extern volatile uint32_t obc_critical_repairs;
extern volatile uint32_t obc_critical_failed_votes;

/*
 * Transitions into an unresolvable state: incremented only when a vote that was
 * resolving stops resolving.
 *
 * **Events, not passages.** That distinction is the whole reason this counter
 * exists rather than the escalation ladder reading `failed_votes`, which counts
 * 66 per corruption in a sixteen-frame window and millions over a soak. A
 * ladder that escalates on the number of times it *noticed* a problem escalates
 * on how often it looked.
 *
 * One irreparable corruption is one event here, however many times it is
 * subsequently observed. It goes back to counting only after a vote resolves
 * again — which, since the voter repairs what it can, means the state genuinely
 * recovered.
 */
extern volatile uint32_t obc_critical_unresolvable_events;

/* Writes all three copies of one item, and their checksums. */
void obc_critical_set(const obc_critical_item_t *item, uint32_t value);

/*
 * Reads through the voter.
 *
 * Returns OBC_OK with the agreed value, repairing any dissenting copy on the
 * way and counting the repair. Returns OBC_ERR_UNSTABLE and leaves *out
 * untouched when no majority exists among the copies that pass their own
 * checksum — which is what two corrupted copies produce. In that case there is
 * no right answer to return, and returning a wrong one would be worse than
 * saying so.
 */
OBC_MUST_CHECK obc_status_t obc_critical_get(const obc_critical_item_t *item,
                                             uint32_t *out);

/*
 * Walks every registered item and repairs a dissenting copy without a caller
 * needing the value.
 *
 * **Today it is redundant, and that is worth stating rather than hiding.** The
 * only registered item is the operating mode, which the dispatch loop reads
 * several times per frame, so read-repair always reaches a corruption first —
 * measured: `repairs` is already 1 by the time the scrubber is first called.
 *
 * It stops being redundant the moment a rarely-read item is registered, which
 * M8's event-log state will be. A cold item corrupted once waits for its next
 * read, possibly hours, and a second corruption in that window turns a
 * recoverable vote into an unresolvable one. The scrubber exists to close that
 * window, and can only be shown to do so once there is cold state to try it on.
 */
OBC_MUST_CHECK obc_status_t obc_critical_scrub(void);

#endif /* OBC_CRITICAL_H */
