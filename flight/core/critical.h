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
 * Seed for the per-copy checksum. Each copy uses a different one, so that a
 * corruption which writes the same pattern over two copies — a burst, a stuck
 * DMA, a wild pointer walking forward — cannot leave both self-consistent.
 * A single shared seed would make identical corruption look like agreement.
 */
#define OBC_CRITICAL_SEED_A 0xA5A50001u
#define OBC_CRITICAL_SEED_B 0x5A5A0002u
#define OBC_CRITICAL_SEED_C 0xC3C30003u

/*
 * The three copies, placed in separate link sections and therefore in separate
 * parts of RAM. See emu/sifive_e.ld: they are interleaved with .bss and .noinit
 * rather than padded apart, which buys the separation without spending memory
 * on the gap.
 */
extern obc_critical_copy_t obc_critical_a;
extern obc_critical_copy_t obc_critical_b;
extern obc_critical_copy_t obc_critical_c;

/* Repairs performed, and votes that could not be resolved. Both are reported:
 * a system that silently repaired a thousand times is not healthy. */
extern volatile uint32_t obc_critical_repairs;
extern volatile uint32_t obc_critical_unresolved;

/* Writes all three copies and their checksums. */
void obc_critical_set(uint32_t value);

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
OBC_MUST_CHECK obc_status_t obc_critical_get(uint32_t *out);

/*
 * Walks the copies and repairs a dissenting one without a caller needing the
 * value. Runs on a period from the task table, so a corruption that is never
 * read still gets found before a second one makes it unrecoverable.
 */
OBC_MUST_CHECK obc_status_t obc_critical_scrub(void);

#endif /* OBC_CRITICAL_H */
