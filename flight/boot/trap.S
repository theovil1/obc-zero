/*
 * Machine-mode trap entry.
 *
 * THE HANDLER NEVER TOUCHES THE STACK. If the fault was a stack overflow or a
 * corrupted sp, saving a context frame would fault again immediately, and the
 * second trap overwrites mepc and mcause — destroying the original cause and
 * leaving only the handler's own fault visible. RISC-V offers no protection
 * against this. The evidence the handler exists to capture is exactly what a
 * stack-using handler loses.
 *
 * So: mscratch holds the address of the fault record, sp is never read or
 * written, and the cause is committed to memory before anything else happens.
 *
 * The handler does not return. M1 records and resets; a resume path arrives
 * with the recovery policy at M3.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/fault.h"

/* AON watchdog, from emu/sifive_e-mtree.txt. Verified to produce a real reset,
 * not a jump to the reset vector. */
#define AON_BASE 0x10000000
#define AON_WDOGCFG 0x000
#define AON_WDOGCOUNT 0x008
#define AON_WDOGKEY 0x01C
#define AON_WDOGCMP0 0x020
#define WDOGKEY_VALUE 0x51F15E
#define WDOGCFG_RSTEN (1 << 8)
#define WDOGCFG_ZEROCMP (1 << 9)
#define WDOGCFG_ENALWAYS (1 << 12)

    .section .text, "ax"
    .align 4                    /* mtvec direct mode ignores the low two bits */
    .global obc_trap_entry
    .type obc_trap_entry, @function
obc_trap_entry:
    /*
     * Swap t0 with mscratch: t0 now holds the record pointer, mscratch holds
     * the interrupted t0. No stack involved.
     */
    csrrw   t0, mscratch, t0

    /* t1 is the only other register used. Park it in the record. */
    sw      t1, OBC_FR_SAVE_T1(t0)

    /*
     * Restore mscratch to the record pointer immediately, parking the
     * interrupted t0 in the record instead.
     *
     * The classic swap idiom relies on the handler swapping back before mret.
     * This handler never returns — it resets — so mscratch would be left
     * holding the interrupted t0, and a *nested* trap would swap that garbage
     * into t0 and write the double-fault marker to an arbitrary address. The
     * double-fault test found exactly that on its first run.
     */
    csrr    t1, mscratch
    sw      t1, OBC_FR_SAVE_T0(t0)
    csrw    mscratch, t0

    /*
     * Re-entry check, before anything else. If the flag is already set, the
     * previous entry never completed and we are inside a double fault. There is
     * nothing left worth trusting, so nothing elaborate is attempted.
     */
    lw      t1, OBC_FR_IN_HANDLER(t0)
    bnez    t1, .Ldouble_fault

    li      t1, 1
    sw      t1, OBC_FR_IN_HANDLER(t0)

    /*
     * Named so the double-fault test can break exactly here: the flag is set,
     * the cause is not yet written. A fault injected at this point is a genuine
     * re-entry, not a first entry that merely looks like one.
     */
    .global obc_trap_flag_set
obc_trap_flag_set:

    /*
     * Commit the cause first, straight from the CSRs. Nothing above this point
     * can fault: every store targets a compile-time RAM address, and this
     * machine has no MMU to make a valid address fail.
     *
     * mcause is stored whole. Bit 31 distinguishes an interrupt from an
     * exception, and cause 3 means two unrelated things depending on it.
     */
    csrr    t1, mcause
    sw      t1, OBC_FR_CAUSE(t0)
    csrr    t1, mepc
    sw      t1, OBC_FR_EPC(t0)
    csrr    t1, mtval
    sw      t1, OBC_FR_TVAL(t0)
    li      t1, OBC_RESET_TRAP
    sw      t1, OBC_FR_RESET_CAUSE(t0)

    /*
     * Checksum, then magic. Never the other way round: the magic is the commit
     * point, so a reset between the two stores leaves a record with no magic,
     * which the reader rejects. Writing the magic first would open a window in
     * which a torn record reads back as authentic.
     */
    li      t1, OBC_FAULT_CHECKSUM_SEED
    lw      t2, OBC_FR_CAUSE(t0)
    xor     t1, t1, t2
    lw      t2, OBC_FR_EPC(t0)
    xor     t1, t1, t2
    lw      t2, OBC_FR_TVAL(t0)
    xor     t1, t1, t2
    lw      t2, OBC_FR_RESET_CAUSE(t0)
    xor     t1, t1, t2
    sw      t1, OBC_FR_CHECKSUM(t0)

    li      t1, OBC_FAULT_MAGIC
    sw      t1, OBC_FR_MAGIC(t0)

    j       obc_reset_now

.Ldouble_fault:
    /*
     * One store, then reset. No payload and no checksum: a double fault means
     * the first handler entry did not survive, so any detail written now would
     * be a claim we cannot support. A 32-bit store is atomic here, so this
     * marker cannot itself be torn.
     */
    li      t1, OBC_FAULT_MAGIC_DOUBLE
    sw      t1, OBC_FR_MAGIC(t0)
    j       obc_reset_now

    .size obc_trap_entry, . - obc_trap_entry


/*
 * Reset through the AON watchdog. A real reset, not a jump to 0x20400000: the
 * jump would re-run the startup code while leaving every peripheral configured,
 * which is a different event and must never be reported as the same one.
 *
 * Clobbers t0 t1 t2 without saving them. Nothing after this point returns.
 */
    .align 4
    .global obc_reset_now
    .type obc_reset_now, @function
obc_reset_now:
    li      t0, AON_BASE
    li      t1, WDOGKEY_VALUE

    /* Each protected register needs the key written immediately before it. */
    sw      t1, AON_WDOGKEY(t0)
    sw      zero, AON_WDOGCOUNT(t0)

    sw      t1, AON_WDOGKEY(t0)
    li      t2, 1
    sw      t2, AON_WDOGCMP0(t0)

    sw      t1, AON_WDOGKEY(t0)
    li      t2, (WDOGCFG_ENALWAYS | WDOGCFG_RSTEN | WDOGCFG_ZEROCMP)
    sw      t2, AON_WDOGCFG(t0)

    /*
     * Spin on nop rather than wfi. The watchdog is in the always-on domain and
     * fires either way, but a bounded-looking spin keeps the failure visible in
     * a trace if the reset ever does not arrive.
     */
1:  nop
    j       1b

    .size obc_reset_now, . - obc_reset_now
