/*
 * Triple-redundant critical state, read through a voter.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/critical.h"

#include <stdint.h>

#include "core/status.h"

/*
 * Separate sections, and therefore separate parts of RAM. The linker script
 * interleaves them with .bss and .noinit so the separation costs nothing: a
 * corruption that walks forward from one copy meets several hundred bytes of
 * unrelated data before it reaches the next.
 */
obc_critical_copy_t obc_critical_a __attribute__((section(".critical0"), used));
obc_critical_copy_t obc_critical_b __attribute__((section(".critical1"), used));
obc_critical_copy_t obc_critical_c __attribute__((section(".critical2"), used));

volatile uint32_t obc_critical_repairs;
volatile uint32_t obc_critical_unresolved;

static uint32_t checksum_of(uint32_t value, uint32_t seed)
{
    /*
     * Not a CRC. This detects a corrupted word, which is the threat; it makes
     * no claim about burst patterns chosen to defeat it, and there is no
     * adversary here to choose one.
     */
    return (value ^ seed) + (value << 7) + (value >> 3);
}

static uint32_t seed_of(const obc_critical_copy_t *copy)
{
    if (copy == &obc_critical_a) {
        return OBC_CRITICAL_SEED_A;
    }
    if (copy == &obc_critical_b) {
        return OBC_CRITICAL_SEED_B;
    }
    return OBC_CRITICAL_SEED_C;
}

static int copy_is_intact(const obc_critical_copy_t *copy)
{
    return copy->checksum == checksum_of(copy->value, seed_of(copy));
}

static void write_copy(obc_critical_copy_t *copy, uint32_t value)
{
    copy->value = value;
    copy->checksum = checksum_of(value, seed_of(copy));
}

void obc_critical_set(uint32_t value)
{
    write_copy(&obc_critical_a, value);
    write_copy(&obc_critical_b, value);
    write_copy(&obc_critical_c, value);
}

/*
 * The vote.
 *
 * Two filters in order, and the order matters. A copy that fails its own
 * checksum is excluded before any comparison, because a corrupted copy that
 * happens to match another corrupted copy would otherwise form a majority of
 * two and win. Only then do the survivors vote.
 */
static obc_status_t vote(uint32_t *out, int repair)
{
    obc_critical_copy_t *copies[OBC_CRITICAL_COPIES] = {
        &obc_critical_a, &obc_critical_b, &obc_critical_c
    };
    uint32_t i;
    uint32_t j;

    for (i = 0u; i < OBC_CRITICAL_COPIES; i++) {
        uint32_t agreeing;

        if (!copy_is_intact(copies[i])) {
            continue;
        }

        agreeing = 1u;
        for (j = 0u; j < OBC_CRITICAL_COPIES; j++) {
            if (j == i) {
                continue;
            }
            if (copy_is_intact(copies[j]) && copies[j]->value == copies[i]->value) {
                agreeing++;
            }
        }

        if (agreeing >= 2u) {
            uint32_t winner = copies[i]->value;

            if (repair) {
                for (j = 0u; j < OBC_CRITICAL_COPIES; j++) {
                    if (!copy_is_intact(copies[j]) || copies[j]->value != winner) {
                        write_copy(copies[j], winner);
                        obc_critical_repairs++;
                    }
                }
            }
            if (out != 0) {
                *out = winner;
            }
            return OBC_OK;
        }
    }

    /*
     * No majority among the intact copies. Two corruptions produce this, and
     * there is no right answer available: returning one of the survivors would
     * be returning a value the system cannot vouch for. Say so instead.
     */
    obc_critical_unresolved++;
    return OBC_ERR_UNSTABLE;
}

obc_status_t obc_critical_get(uint32_t *out)
{
    if (out == 0) {
        return OBC_ERR_INVALID;
    }
    return vote(out, 1);
}

obc_status_t obc_critical_scrub(void)
{
    /*
     * The same vote, without a caller wanting the value. State that is written
     * once and read rarely would otherwise accumulate corruptions until a
     * second one made the first unrecoverable, and the scrubber exists to find
     * the first while a majority still survives it.
     */
    return vote(0, 1);
}
