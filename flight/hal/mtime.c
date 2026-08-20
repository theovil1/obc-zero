/*
 * Machine timer, RV32 64-bit read.
 *
 * Addresses verified against the machine, not taken from the standard SiFive
 * layout: emu/sifive_e-mtree.txt places the ACLINT MTIMER region at
 * 0x02004000-0x0200bfff, and mtime was confirmed to advance at 0x0200BFF8.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hal/mtime.h"

#include <stdint.h>

#include "core/status.h"

#define MTIME_LO 0x0200BFF8u
#define MTIME_HI 0x0200BFFCu

/*
 * Retry budget for the high / low / re-read-high sequence.
 *
 * Two iterations are sufficient: the high word can only move once per 2^32
 * ticks, so a second attempt cannot race a second carry. That is an argument,
 * not a bound, and an argument is not what this project runs on. Four caps it
 * with margin and turns "the timer is behaving impossibly" into a status the
 * caller has to handle rather than a loop that never returns.
 */
#define MTIME_READ_ATTEMPTS 4u

static uint32_t s_unstable_count;

static inline uint32_t reg_read(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

obc_status_t obc_mtime_read(uint64_t *out)
{
    uint32_t attempt;

    if (out == 0) {
        return OBC_ERR_INVALID;
    }

    for (attempt = 0u; attempt < MTIME_READ_ATTEMPTS; attempt++) {
        uint32_t hi = reg_read(MTIME_HI);
        uint32_t lo = reg_read(MTIME_LO); /* CARRY-INJECT */
        uint32_t hi_again = reg_read(MTIME_HI);

        /*
         * If the high word did not move across the low read, the two halves
         * belong to the same instant. If it did, the low word we hold is from
         * the wrong side of a carry and the whole read is discarded.
         */
        if (hi == hi_again) {
            *out = ((uint64_t)hi << 32) | (uint64_t)lo;
            return OBC_OK;
        }
    }

    s_unstable_count++;
    return OBC_ERR_UNSTABLE;
}

uint32_t obc_mtime_unstable_count(void)
{
    return s_unstable_count;
}
