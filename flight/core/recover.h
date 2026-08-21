/*
 * Escalation, and the watchdog that drives it.
 *
 * Shape fixed in docs/adr/0007-escalation.md before this was written: two rungs
 * with the third named and empty, a window counted in boots because no clock
 * spans a reset, and safe mode as the top of the ladder rather than a parallel
 * mechanism. The ladder escalates towards safe mode and never away from it.
 *
 * **Nothing here runs as a task.** A watchdog dispatched from the table is fed
 * by, checked by, and escalated by the same executive it is supposed to be
 * watching: a failure of that executive takes the watchdog with it, leaving a
 * mechanism that only works in the cases where everything else already does.
 * That is the system-level shape of the M1 mscratch defect — the path that
 * matters is the one that runs when the rest has failed.
 *
 * So the ladder lives in the frame loop, and the backstop underneath it is the
 * AON hardware watchdog, which is in the always-on domain and does not care
 * whether any software is still running.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OBC_RECOVER_H
#define OBC_RECOVER_H

#include <stdint.h>

#include "core/status.h"

/*
 * The rungs.
 *
 * Rung 2 is declared and does nothing. A subsystem reset needs a subsystem with
 * its own state, and none exists before M6 and M8. Inventing something to put
 * here would read as escalation while doing nothing, which is visible only in a
 * campaign; an empty rung is visible in the ladder itself.
 */
#define OBC_RUNG_NONE 0u
#define OBC_RUNG_SUSPEND_TASK 1u
#define OBC_RUNG_RESET_SUBSYSTEM 2u /* empty until M6/M8, nothing selects it */
#define OBC_RUNG_RESET_MACHINE 3u

/*
 * Suspension log: which task was suspended, and in which frame.
 *
 * This is not bookkeeping. Rung 1 suspends a task by not dispatching it, which
 * on a task with no persistent state is indistinguishable from the scheduler
 * failing to dispatch it — and M2's conformance checker correctly calls that a
 * dropped dispatch. The log is what tells the two apart.
 *
 * It is authoritative and written at the moment of suspension, so the checker
 * can require both directions: every gap has a matching entry, and every entry
 * has a matching gap. Keyed on (task, frame) so that a suspension recorded in
 * one frame cannot excuse a dispatch dropped in another.
 */
#define OBC_SUSPEND_LOG_LEN 16u

typedef struct {
    uint8_t task;
    uint8_t frame;
    uint8_t rung;
    uint8_t pad;
} obc_suspension_t;

extern volatile obc_suspension_t obc_suspensions[OBC_SUSPEND_LOG_LEN];
extern volatile uint32_t obc_suspension_count;
extern volatile uint32_t obc_suspension_overflow;

/*
 * Consecutive short boots, in a record of its own.
 *
 * Its own magic and its own checksum, because it must survive
 * obc_fault_consume(): the fault record is cleared at every boot, so a field
 * inside it could not answer "how many boots in a row failed to stay up".
 *
 * **Counted by a flag raised at boot and lowered on success**, not by an
 * increment on the reset path.
 *
 * The reset-path version was written first and tested against a hung executive,
 * where the AON watchdog resets the machine **in hardware, with no software
 * running**. Nothing incremented anything, the count stayed at zero, and the
 * system would have looped forever without ever noticing — the precise failure
 * this exists to catch, arriving through the one door a software counter cannot
 * cover.
 *
 * So: `in_progress` is raised as early as the boot can raise it and lowered
 * only when the boot is declared healthy. A boot that finds it still raised
 * knows the previous one died, however it died and however early.
 *
 * The irreducible limit, stated rather than hidden: a boot that dies *before*
 * the flag is raised is not counted. That window is the startup code between
 * the reset vector and the first C statement, and closing it would need a
 * counter the hardware maintains.
 */
#define OBC_BOOT_MAGIC 0x0BC8007Au
#define OBC_BOOT_CHECKSUM_SEED 0x7A00BC0Bu

typedef struct {
    volatile uint32_t magic;
    volatile uint32_t checksum;
    volatile uint32_t short_boots;
    volatile uint32_t in_progress;
} obc_boot_record_t;

extern obc_boot_record_t obc_boot_record;

/*
 * A boot shorter than this counts towards the loop. Arbitrary, and declared so:
 * calibrating it needs a mission profile, which does not exist. Recorded in
 * ADR 0007 as a figure to revisit rather than one derived from anything.
 */
#define OBC_SHORT_BOOT_LIMIT 5u

/*
 * What "the boot succeeded" means, and it is the definition the whole mechanism
 * rests on.
 *
 * **Not the end of initialisation.** A system that initialises perfectly and
 * then dies on its first frame would lower the flag every time, reset the
 * streak every time, and never trigger the protection — while looping forever.
 * Counting boots that *start* is not counting boots that *work*.
 *
 * So the flag is lowered on evidence that the executive is alive: eight
 * complete frames, a quarter of a second at 32768 Hz. Eight rather than one
 * because a single frame proves the loop was entered, not that it turns; and it
 * is the period of the slowest task in the table, so every task has been
 * dispatched at least once before the boot is called a success.
 *
 * That last property is why the number is not arbitrary. If the table gains a
 * slower task, this has to grow with it, and the static assertion in tasks.c
 * says so.
 */
#define OBC_HEALTHY_FRAMES 8u

/* Reads the surviving count, rejecting a record that is not intact. */
uint32_t obc_recover_short_boots(void);

/*
 * Raises the in-progress flag, counting the previous boot if it never lowered
 * it. Called as early in the boot as it can be, because everything before it is
 * an uncounted window.
 *
 * Returns the rung the caller should take: OBC_RUNG_NONE normally, or
 * OBC_RUNG_RESET_MACHINE once the streak has reached its limit — at which point
 * escalating enters safe mode rather than resetting again.
 */
uint32_t obc_recover_boot_begins(void);

/*
 * Lowers the flag if the executive has shown OBC_HEALTHY_FRAMES of life, and
 * does nothing otherwise. Clears the streak: the window is *consecutive* short
 * boots, not a lifetime total.
 *
 * Returns non-zero if the boot was declared healthy, so a caller can report
 * which of the two happened rather than assume.
 */
int obc_recover_boot_is_healthy(uint32_t frames_completed);

/*
 * Escalates. Records the rung, acts on it, and never returns for rung 3.
 *
 * Rung 1 suspends the task for the next frame. Rung 3 increments the short-boot
 * count and resets — unless the count has already reached its limit, in which
 * case it enters safe mode instead. A reset that has failed five times running
 * will not work on the sixth, and continuing costs the ability to be observed.
 */
void obc_recover_escalate(uint32_t rung, uint32_t task_index, uint32_t frame);

/* True if this task is suspended for the current frame. */
int obc_recover_is_suspended(uint32_t task_index, uint32_t frame);

/* Arms the hardware backstop, and feeds it. Neither is a task: a watchdog fed
 * by the thing it watches is not a watchdog. */
void obc_recover_arm_hardware(void);
void obc_recover_feed_hardware(void);

/*
 * Disarms the backstop.
 *
 * Paired with arming: the executive owns the watchdog for exactly as long as it
 * runs. M2's window is bounded, so the executive returns and then nothing feeds
 * — without this, every run resets once the window closes, which is a reset
 * caused by the test harness rather than by anything under test.
 *
 * In flight the executive does not return and this is never called. The pairing
 * is what keeps that true by construction rather than by hoping.
 */
void obc_recover_disarm_hardware(void);

#endif /* OBC_RECOVER_H */
