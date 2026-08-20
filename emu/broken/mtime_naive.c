/*
 * DELIBERATELY BROKEN. Not flight code. Never linked into a flight image.
 *
 * This file exists so that the carry test can be shown to fail before it is
 * trusted. A test for a race that never fires is indistinguishable from a test
 * that fires and passes. Built only by `make test-mtime-broken`, which asserts
 * that the carry test FAILS against this implementation.
 *
 * The defect: read the low word, then the high word, and return them together.
 * If the low word carries between the two reads, the returned value pairs the
 * old low word with the new high word and is about 2^32 **too large**.
 *
 * That direction is the whole point. There are two naive orderings and they
 * fail differently:
 *
 *   high then low  -> value is ~2^32 too *small*, the counter appears to jump
 *                     backwards, and a monotonicity assertion catches it.
 *   low then high  -> value is ~2^32 too *large*, the counter still only ever
 *                     increases, and a monotonicity assertion passes happily
 *                     on a clock that is catastrophically wrong.
 *
 * This file implements the second one on purpose. It is the variant that
 * defeats the obvious criterion, which is why the acceptance criterion asserts
 * *bounded progression* rather than mere monotonicity.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hal/mtime.h"

#include <stdint.h>

#include "core/status.h"

#define MTIME_LO 0x0200BFF8u
#define MTIME_HI 0x0200BFFCu

static inline uint32_t reg_read(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

obc_status_t obc_mtime_read(uint64_t *out)
{
    uint32_t lo;
    uint32_t hi;

    if (out == 0) {
        return OBC_ERR_INVALID;
    }

    lo = reg_read(MTIME_LO);
    hi = reg_read(MTIME_HI); /* CARRY-INJECT */

    *out = ((uint64_t)hi << 32) | (uint64_t)lo;
    return OBC_OK;
}

uint32_t obc_mtime_unstable_count(void)
{
    return 0u;
}
