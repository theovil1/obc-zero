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
