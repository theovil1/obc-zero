/*
 * DELIBERATELY BROKEN. Not flight code. Never linked into a flight image.
 *
 * A machine timer that reads correctly for a while and then refuses to settle,
 * returning OBC_ERR_UNSTABLE for good. It exists to reach safe mode through the
 * clock, which is the one of the three entry points that cannot be provoked
 * from outside: the retry loop is internal to obc_mtime_read, and no debugger
 * write can make the high word disagree with itself across three reads.
 *
 * The failure is deliberately late rather than immediate. A timer broken at
 * boot would be caught by the timebase assertion before the executive ever
 * runs, which proves that assertion works and proves nothing about safe mode.
 *
 * Built only by `make test-safe-clock`.
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
 * Measured, not guessed. A full nominal run performs 166817 reads across the
 * 16-frame window, about 10400 per frame, of which roughly 6000 are consumed by
 * the boot checks before the executive starts.
 *
 * 100000 therefore lands around frame 9: comfortably past the boot checks, and
 * comfortably inside the window. The first attempt used 200000, which is more
 * than the whole run performs, so the clock never failed and the test passed
 * while proving nothing.
 */
#define BROKEN_READS_BEFORE_FAILURE 100000u

static uint32_t s_reads;
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

    if (s_reads < BROKEN_READS_BEFORE_FAILURE) {
        s_reads++;
    } else {
        s_unstable_count++;
        return OBC_ERR_UNSTABLE;
    }

    for (attempt = 0u; attempt < 4u; attempt++) {
        uint32_t hi = reg_read(MTIME_HI);
        uint32_t lo = reg_read(MTIME_LO);
        uint32_t hi_again = reg_read(MTIME_HI);

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
