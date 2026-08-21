/*
 * Fault record validation.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/fault.h"

#include <stddef.h>
#include <stdint.h>

#include "core/status.h"

/*
 * The record itself. In .noinit, so the startup code never zeroes it and it
 * survives a warm reset. `used` keeps it against --gc-sections: the trap
 * handler reaches it through mscratch, which the linker cannot see.
 */
obc_fault_record_t obc_fault_record __attribute__((section(".noinit"), used));

/*
 * The assembly handler addresses this structure by hardcoded byte offsets. If a
 * field moves, the handler writes to the wrong word and the failure is silent
 * and total. Break the build instead.
 */
_Static_assert(offsetof(obc_fault_record_t, magic) == OBC_FR_MAGIC, "magic");
_Static_assert(offsetof(obc_fault_record_t, checksum) == OBC_FR_CHECKSUM, "cksum");
_Static_assert(offsetof(obc_fault_record_t, cause) == OBC_FR_CAUSE, "cause");
_Static_assert(offsetof(obc_fault_record_t, epc) == OBC_FR_EPC, "epc");
_Static_assert(offsetof(obc_fault_record_t, tval) == OBC_FR_TVAL, "tval");
_Static_assert(offsetof(obc_fault_record_t, reset_cause) == OBC_FR_RESET_CAUSE,
               "reset_cause");
_Static_assert(offsetof(obc_fault_record_t, in_handler) == OBC_FR_IN_HANDLER,
               "in_handler");
_Static_assert(offsetof(obc_fault_record_t, save_t1) == OBC_FR_SAVE_T1, "save_t1");
_Static_assert(offsetof(obc_fault_record_t, save_t0) == OBC_FR_SAVE_T0, "save_t0");
_Static_assert(sizeof(obc_fault_record_t) == OBC_FR_SIZE, "record size");

/*
 * XOR over the four payload words.
 *
 * in_handler is deliberately excluded. It is cleared by the startup code on
 * every boot, so including it would make a valid record fail its own checksum
 * the moment it was read back. The two fields answer different questions and
 * are protected differently: see the comment in flight/boot/start.S.
 *
 * save_t1 is excluded for the same reason — it is handler scratch with no
 * meaning between traps.
 */
uint32_t obc_fault_checksum(const obc_fault_record_t *r)
{
    if (r == NULL) {
        return 0u;
    }
    return OBC_FAULT_CHECKSUM_SEED ^ r->cause ^ r->epc ^ r->tval ^ r->reset_cause;
}

obc_status_t obc_fault_validate(uint32_t *out_reset_cause)
{
    uint32_t magic;

    if (out_reset_cause == NULL) {
        return OBC_ERR_INVALID;
    }
    *out_reset_cause = OBC_RESET_UNKNOWN;

    magic = obc_fault_record.magic;

    /*
     * A double-fault marker carries no payload and is not checksummed, on
     * purpose: inside a double fault nothing else was worth trusting. Report
     * the fact and claim nothing more.
     */
    if (magic == OBC_FAULT_MAGIC_DOUBLE) {
        *out_reset_cause = OBC_RESET_DOUBLE_FAULT;
        return OBC_OK;
    }

    /*
     * Anything else that is not the magic is not a record. A cold boot lands
     * here, and so does a reset that interrupted the handler before it reached
     * the final store — which is exactly why the magic is written last.
     */
    if (magic != OBC_FAULT_MAGIC) {
        return OBC_ERR_INVALID;
    }

    /*
     * Magic right, checksum wrong: a real record that has been corrupted,
     * which is a different event from no record at all and is reported as one.
     * Its contents are not returned; a record that fails its own checksum has
     * no fields worth reading.
     */
    if (obc_fault_record.checksum != obc_fault_checksum(&obc_fault_record)) {
        return OBC_ERR_UNSTABLE;
    }

    *out_reset_cause = obc_fault_record.reset_cause;
    return OBC_OK;
}

void obc_fault_consume(void)
{
    /* Magic first, in reverse of the write order: clearing the commit point
     * before the payload means no window exposes a record that looks valid. */
    obc_fault_record.magic = 0u;
    obc_fault_record.checksum = 0u;
    obc_fault_record.cause = 0u;
    obc_fault_record.epc = 0u;
    obc_fault_record.tval = 0u;
    obc_fault_record.reset_cause = OBC_RESET_UNKNOWN;
}
