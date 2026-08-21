/*
 * Machine timer. The only real-time source in the system.
 *
 * Authority, per docs/adr/0002-time-domains.md: this domain owns scheduling,
 * deadlines, timestamps and every duration that is ever published. It does not
 * own per-task execution budgets, which are measured in retired instructions
 * because one tick here is 30.5 us and a task can run in less.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OBC_HAL_MTIME_H
#define OBC_HAL_MTIME_H

#include <stdint.h>

#include "core/status.h"

/*
 * Timebase, measured against wall clock rather than taken from documentation:
 * 32768 ticks elapse in 1.04 s. See docs/adr/0001-target-platform.md.
 *
 * One tick is 30.5 us.
 */
#define OBC_MTIME_HZ 32768u

/*
 * Reads the 64-bit machine timer.
 *
 * On RV32 the counter is two 32-bit registers, so a read is not atomic. The
 * sequence is high / low / re-read high, retried if the high word moved between
 * the two reads. Reading low-then-high, or high-then-low without the recheck,
 * yields a value that is wrong by about 2^32 at every carry.
 *
 * Returns OBC_ERR_UNSTABLE if the retry budget is exhausted. That is a recorded
 * fault, not a condition to wait out: two iterations suffice unless the timer
 * is behaving in a way the design does not account for, and in that case the
 * caller must escalate rather than spin.
 *
 * *out is left untouched on failure.
 */
OBC_MUST_CHECK obc_status_t obc_mtime_read(uint64_t *out);

/* Number of times obc_mtime_read exhausted its retry budget since reset.
 * Non-zero means the timer misbehaved and the system should say so. */
uint32_t obc_mtime_unstable_count(void);

#endif /* OBC_HAL_MTIME_H */
