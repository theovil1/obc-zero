/*
 * Operating mode.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/mode.h"

#include <stdint.h>

#include "core/critical.h"
#include "core/fault.h"
#include "core/status.h"
#include "hal/uart.h"

volatile uint32_t obc_mode;
volatile uint32_t obc_safe_reason;

/*
 * Guards the announcement, not the mode.
 *
 * obc_mode_enter_safe cannot ask obc_mode_is_safe whether it has already run:
 * a broken vote makes that function answer "safe" by design, which would
 * suppress the very announcement that says the vote broke. A plain latch is the
 * right tool — it is not critical state, it decides nothing about behaviour,
 * and it only stops the same line being printed twice.
 */
static volatile uint32_t s_announced;

int obc_mode_is_safe(void)
{
    uint32_t value = OBC_MODE_SAFE;
    obc_status_t st = obc_critical_get(&obc_critical_mode, &value);

    if (st != OBC_OK) {
        /* No majority. Fail safe: see the header for why the two guesses are
         * not symmetric. */
        obc_mode = OBC_MODE_SAFE;
        return 1;
    }

    obc_mode = value; /* refresh the mirror the host reads */
    return value == OBC_MODE_SAFE;
}

obc_mode_record_t obc_mode_record __attribute__((section(".noinit"), used));

static uint32_t mode_checksum(const obc_mode_record_t *r)
{
    return OBC_MODE_CHECKSUM_SEED ^ r->reason;
}

static const char *reason_text(uint32_t reason)
{
    if (reason == OBC_SAFE_TRAP) {
        return "trap on a previous boot";
    }
    if (reason == OBC_SAFE_FRAME_OVERRUN) {
        return "frame overrun";
    }
    if (reason == OBC_SAFE_CLOCK) {
        return "clock would not settle";
    }
    if (reason == OBC_SAFE_RESET_LOOP) {
        return "reset loop, the ladder ran out of rungs";
    }
    return "unspecified";
}

void obc_mode_enter_safe(uint32_t reason)
{
    /*
     * Keep the first reason. A frame overrun caused by a stopped clock would
     * otherwise overwrite the clock as the explanation, leaving the symptom
     * recorded and the cause lost.
     */
    if (s_announced != 0u) {
        return;
    }
    s_announced = 1u;

    obc_critical_set(&obc_critical_mode, OBC_MODE_SAFE);
    obc_mode = OBC_MODE_SAFE;
    obc_safe_reason = reason;

    /* Payload, then checksum, then magic — the same commit order as the fault
     * record, so a reset mid-write leaves nothing that reads as authentic. */
    obc_mode_record.magic = 0u;
    obc_mode_record.reason = reason;
    obc_mode_record.checksum = mode_checksum(&obc_mode_record);
    obc_mode_record.magic = OBC_MODE_MAGIC;

    /*
     * Observable 2 of 3: the serial line. Status discarded deliberately — if
     * the UART is what failed, there is nowhere left to report that it failed,
     * and refusing to enter safe mode because the announcement did not go out
     * would be the wrong trade.
     */
    OBC_IGNORE(obc_uart_puts("mode   : SAFE, "));
    OBC_IGNORE(obc_uart_puts(reason_text(reason)));
    OBC_IGNORE(obc_uart_puts("\r\n"));
}

void obc_mode_restore(uint32_t previous_reset_cause)
{
    obc_critical_set(&obc_critical_mode, OBC_MODE_NOMINAL);
    obc_mode = OBC_MODE_NOMINAL;
    obc_safe_reason = OBC_SAFE_NONE;
    s_announced = 0u;

    /*
     * A trap on the previous boot means this one comes up degraded. The M1
     * handler touches no stack and calls nothing fallible, so it records and
     * resets rather than running policy; this is where the policy it deferred
     * happens.
     */
    if (previous_reset_cause == OBC_RESET_TRAP
        || previous_reset_cause == OBC_RESET_DOUBLE_FAULT) {
        obc_mode_enter_safe(OBC_SAFE_TRAP);
        return;
    }

    /*
     * Otherwise the record is only believed if it is intact. At cold boot this
     * region holds whatever the RAM held, which on silicon is noise, so an
     * unprotected read would put the system into safe mode on the strength of
     * a random word. Magic and checksum both, as for the fault record.
     */
    if (obc_mode_record.magic == OBC_MODE_MAGIC
        && obc_mode_record.checksum == mode_checksum(&obc_mode_record)) {
        obc_mode_enter_safe(obc_mode_record.reason);
    }
}
