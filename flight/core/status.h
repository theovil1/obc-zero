/*
 * Status codes returned by fallible flight functions.
 *
 * Deliberately minimal at M0: only the codes that existing call sites can
 * actually return. M3 defines the full error model, the warn_unused_result
 * enforcement and the panic path. Do not add speculative codes here before
 * something returns them.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OBC_STATUS_H
#define OBC_STATUS_H

/*
 * Marks a return value the caller must look at.
 *
 * The rule "check every return value" has been in force since M0 and was
 * enforced by review, which is to say by nobody. This makes it a build failure,
 * on the same principle as deps-check and the size reference: a discipline that
 * cannot be observed is not a property.
 *
 * Applied only to functions that already return obc_status_t. Widening the set
 * is a separate decision, and a sudden crop of warnings is the signal that the
 * scope has drifted rather than that the codebase is careless.
 */
#define OBC_MUST_CHECK __attribute__((warn_unused_result))

/*
 * Deliberately discards a status, and says so.
 *
 * A `(void)` cast does not silence warn_unused_result under GCC, which turns
 * out to be the better behaviour: it forces every discard to be written as one.
 * Each use of this macro is a claim that there is genuinely nowhere for the
 * status to go, and each is greppable — `grep -c OBC_IGNORE` is the count of
 * places where the system knowingly stops caring, which is a number worth being
 * able to read.
 */
#define OBC_IGNORE(expr)                 \
    do {                                 \
        obc_status_t obc_ignored_ = (expr); \
        (void)obc_ignored_;              \
    } while (0)

typedef enum {
    OBC_OK = 0,
    OBC_ERR_TIMEOUT = 1,
    OBC_ERR_INVALID = 2,
    /* A value that should have settled within a bounded number of attempts did
     * not. Distinct from OBC_ERR_TIMEOUT: nothing was waited for, a retry
     * budget was exhausted. */
    OBC_ERR_UNSTABLE = 3
} obc_status_t;

#endif /* OBC_STATUS_H */
