/*
 * Triple-redundant critical state, read through a voter.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/critical.h"

#include <stddef.h>
#include <stdint.h>

#include "core/status.h"

/*
 * The three regions. Separate link sections, and therefore separate parts of
 * RAM: emu/sifive_e.ld interleaves them with .bss and .noinit and pads the
 * remaining gap, so the copies of any item sit several hundred bytes apart.
 *
 * New critical state adds its copies to these same three sections. The
 * separation is a property of the regions and is paid for once.
 */
static obc_critical_copy_t mode_copy_0 __attribute__((section(".critical0"), used));
static obc_critical_copy_t mode_copy_1 __attribute__((section(".critical1"), used));
static obc_critical_copy_t mode_copy_2 __attribute__((section(".critical2"), used));

const obc_critical_item_t obc_critical_mode = {
    .name = "mode",
    .copies = { &mode_copy_0, &mode_copy_1, &mode_copy_2 },
    .seeds = { OBC_CRITICAL_SEED_0, OBC_CRITICAL_SEED_1, OBC_CRITICAL_SEED_2 },
};

const obc_critical_item_t *const obc_critical_items[] = { &obc_critical_mode };
const uint32_t obc_critical_item_count =
    (uint32_t)(sizeof(obc_critical_items) / sizeof(obc_critical_items[0]));

volatile uint32_t obc_critical_repairs;
volatile uint32_t obc_critical_unresolved;

static uint32_t checksum_of(uint32_t value, uint32_t seed)
{
    /*
     * Not a CRC. This detects a corrupted word, which is the threat; it makes
     * no claim about patterns chosen to defeat it, and there is no adversary
     * here to choose one.
     */
    return (value ^ seed) + (value << 7) + (value >> 3);
}

static int copy_is_intact(const obc_critical_item_t *item, uint32_t index)
{
    const obc_critical_copy_t *copy = item->copies[index];

    return copy->checksum == checksum_of(copy->value, item->seeds[index]);
}

static void write_copy(const obc_critical_item_t *item, uint32_t index,
                       uint32_t value)
{
    item->copies[index]->value = value;
    item->copies[index]->checksum = checksum_of(value, item->seeds[index]);
}

void obc_critical_set(const obc_critical_item_t *item, uint32_t value)
{
    uint32_t i;

    if (item == NULL) {
        return;
    }
    for (i = 0u; i < OBC_CRITICAL_COPIES; i++) {
        write_copy(item, i, value);
    }
}

/*
 * The vote.
 *
 * Two filters in order, and the order matters. A copy that fails its own
 * checksum is excluded before any comparison, because a corrupted copy that
 * happens to match another corrupted copy would otherwise form a majority of
 * two and win against the one surviving good copy. Only then do the survivors
 * vote.
 */
static obc_status_t vote(const obc_critical_item_t *item, uint32_t *out)
{
    uint32_t i;
    uint32_t j;

    for (i = 0u; i < OBC_CRITICAL_COPIES; i++) {
        uint32_t agreeing;

        if (!copy_is_intact(item, i)) {
            continue;
        }

        agreeing = 1u;
        for (j = 0u; j < OBC_CRITICAL_COPIES; j++) {
            if (j == i) {
                continue;
            }
            if (copy_is_intact(item, j)
                && item->copies[j]->value == item->copies[i]->value) {
                agreeing++;
            }
        }

        if (agreeing >= 2u) {
            uint32_t winner = item->copies[i]->value;

            for (j = 0u; j < OBC_CRITICAL_COPIES; j++) {
                if (!copy_is_intact(item, j)
                    || item->copies[j]->value != winner) {
                    write_copy(item, j, winner);
                    obc_critical_repairs++;
                }
            }
            if (out != NULL) {
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

obc_status_t obc_critical_get(const obc_critical_item_t *item, uint32_t *out)
{
    if (item == NULL || out == NULL) {
        return OBC_ERR_INVALID;
    }
    return vote(item, out);
}

obc_status_t obc_critical_scrub(void)
{
    obc_status_t worst = OBC_OK;
    uint32_t i;

    /*
     * Every registered item, not only the one a caller happens to want. Bounded
     * by the registry, which is a compile-time constant in flash.
     */
    for (i = 0u; i < obc_critical_item_count; i++) {
        obc_status_t st = vote(obc_critical_items[i], NULL);

        if (st != OBC_OK) {
            worst = st;
        }
    }
    return worst;
}
