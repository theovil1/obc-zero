/*
 * Fault record: what the trap handler writes, and what the next boot reads.
 *
 * Lives in .noinit, which the startup code never zeroes, so it survives a warm
 * reset. That is the only reason it is readable at all — see
 * docs/adr/0001-target-platform.md for why it is in RAM rather than in the AON
 * backup registers a real FE310 would use.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OBC_FAULT_H
#define OBC_FAULT_H

/*
 * Byte offsets into obc_fault_record. Defined as macros because the trap
 * handler is assembly and cannot see the C layout; static assertions in
 * fault.c check that the two agree, so a reordered field breaks the build
 * rather than the handler.
 */
#define OBC_FR_MAGIC 0
#define OBC_FR_CHECKSUM 4
#define OBC_FR_CAUSE 8
#define OBC_FR_EPC 12
#define OBC_FR_TVAL 16
#define OBC_FR_RESET_CAUSE 20
#define OBC_FR_IN_HANDLER 24
#define OBC_FR_SAVE_T1 28
#define OBC_FR_SAVE_T0 32
#define OBC_FR_SIZE 36

/*
 * Magic values. The magic is written LAST, after payload and checksum, so it is
 * the commit point: a record carrying it is a record whose payload was already
 * complete. A reset landing mid-write leaves no magic and the record is
 * correctly rejected rather than half-believed.
 */
#define OBC_FAULT_MAGIC 0x0BCFA017u

/*
 * Double fault. Written as a single store and nothing else — no payload, no
 * checksum. Inside a double fault there is nothing left worth trusting, so the
 * record claims only the fact that it happened. A 32-bit store is atomic on
 * RV32, so this cannot itself be torn.
 */
#define OBC_FAULT_MAGIC_DOUBLE 0x0BCDEAD2u

/*
 * Checksum seed. The checksum is a XOR of the four payload words with this
 * seed: cheap enough to compute in the handler without a stack or a loop.
 *
 * It is a garbage detector, not an error-correcting code. Against uniformly
 * random RAM it accepts a false record with probability 2^-32, which is what it
 * is for. It offers no protection against a correlated multi-word corruption
 * that happens to preserve the XOR, and that limitation is stated rather than
 * papered over.
 */
#define OBC_FAULT_CHECKSUM_SEED 0x5A5AC3C3u

/* Values for the reset_cause field. */
#define OBC_RESET_UNKNOWN 0u
#define OBC_RESET_TRAP 1u
#define OBC_RESET_DOUBLE_FAULT 2u
/* A reset the software asked for, through the watchdog, with nothing wrong.
 * Distinct from a trap so that a deliberate restart is never read as a fault.
 * M5 adds the policy that decides when to ask; M1 provides only the path. */
#define OBC_RESET_REQUESTED 3u

#ifndef __ASSEMBLER__

#include <stdint.h>

#include "core/status.h"

typedef struct {
    volatile uint32_t magic;
    volatile uint32_t checksum;
    volatile uint32_t cause; /* full mcause: the interrupt bit is bit 31 and
                              * must never be truncated away, see below */
    volatile uint32_t epc;
    volatile uint32_t tval;
    volatile uint32_t reset_cause;
    volatile uint32_t in_handler; /* NOT covered by the checksum, see fault.c */
    volatile uint32_t save_t1;    /* handler scratch, no meaning between traps */
    volatile uint32_t save_t0;    /* interrupted t0, parked here so mscratch can be
                                   * restored immediately, see flight/boot/trap.S */
} obc_fault_record_t;

extern obc_fault_record_t obc_fault_record;

/*
 * The previous boot's reset cause, latched before the record is consumed.
 *
 * obc_fault_consume() destroys the record, so the cause is readable exactly
 * once, early in main(). It was already computed there and then dropped;
 * telemetry needs it every frame, and re-reading a consumed record would return
 * OBC_RESET_UNKNOWN for the rest of the mission — a frame confidently reporting
 * that nothing happened.
 */
extern volatile uint32_t obc_reset_cause_previous;

/*
 * mcause carries the interrupt flag in bit 31. Exception cause 3 (breakpoint)
 * and interrupt cause 3 (machine software interrupt) are unrelated events that
 * share a code, so the field is kept 32 bits wide everywhere and these two
 * helpers are the only sanctioned way to take it apart.
 */
#define OBC_MCAUSE_INTERRUPT_BIT 0x80000000u

static inline int obc_mcause_is_interrupt(uint32_t mcause)
{
    return (mcause & OBC_MCAUSE_INTERRUPT_BIT) != 0u;
}

static inline uint32_t obc_mcause_code(uint32_t mcause)
{
    return mcause & ~OBC_MCAUSE_INTERRUPT_BIT;
}

/*
 * Validates the record left by the previous boot.
 *
 * Returns OBC_OK if the record is authentic, OBC_ERR_INVALID if the magic is
 * wrong (including a cold boot, where the region holds whatever RAM held), and
 * OBC_ERR_UNSTABLE if the magic is right but the checksum disagrees — which
 * means a real record was corrupted rather than absent, and is worth reporting
 * differently.
 *
 * *out_reset_cause receives OBC_RESET_DOUBLE_FAULT for a double-fault record;
 * no other field of such a record is meaningful and none is returned.
 */
OBC_MUST_CHECK obc_status_t obc_fault_validate(uint32_t *out_reset_cause);

/*
 * Records `cause` and resets through the AON watchdog. Does not return.
 *
 * This is the deliberate-reset path, reached from ordinary context rather than
 * from a fault, so unlike the trap handler it may use the stack. It shares the
 * same write order — payload, checksum, magic — and the same reset mechanism.
 */
void obc_fault_reset_with_cause(uint32_t cause) __attribute__((noreturn));

/* Invalidates the record so the next boot does not read a stale one. Clears the
 * magic first, which is the commit point in reverse. */
void obc_fault_consume(void);

/* The checksum the payload should carry. Exposed so tests can corrupt exactly
 * one field and leave the other correct. */
uint32_t obc_fault_checksum(const obc_fault_record_t *r);

#endif /* __ASSEMBLER__ */

#endif /* OBC_FAULT_H */
