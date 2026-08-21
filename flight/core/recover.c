/*
 * Escalation, and the watchdog that drives it.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/recover.h"

#include <stdint.h>

#include "core/mode.h"
#include "core/sched.h"
#include "core/status.h"
#include "hal/uart.h"

/* AON watchdog, verified against the machine in ADR 0001. */
#define AON_BASE 0x10000000u
#define AON_WDOGCFG 0x000u
#define AON_WDOGCOUNT 0x008u
#define AON_WDOGKEY 0x01Cu
#define AON_WDOGCMP0 0x020u
#define WDOGKEY_VALUE 0x51F15Eu
#define WDOGCFG_RSTEN (1u << 8)
#define WDOGCFG_ZEROCMP (1u << 9)
#define WDOGCFG_ENALWAYS (1u << 12)

/*
 * Hardware timeout, in always-on clock ticks. Four minor frames: long enough
 * that a healthy executive always feeds in time, short enough that a hung one
 * is reset before a ground station would notice.
 */
#define WDOG_TIMEOUT_TICKS 4096u

volatile obc_suspension_t obc_suspensions[OBC_SUSPEND_LOG_LEN];
volatile uint32_t obc_suspension_count;
volatile uint32_t obc_suspension_overflow;

obc_boot_record_t obc_boot_record __attribute__((section(".noinit"), used));

/* Set by escalate(), read by the dispatch loop. Not persistent: a suspension
 * lasts one frame, and a reset ends it along with everything else. */
static volatile uint32_t s_suspended_task = 0xFFFFFFFFu;
static volatile uint32_t s_suspended_frame = 0xFFFFFFFFu;

void obc_reset_now(void) __attribute__((noreturn));

static void reg_write(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)(AON_BASE + offset) = value;
}

void obc_recover_arm_hardware(void)
{
    reg_write(AON_WDOGKEY, WDOGKEY_VALUE);
    reg_write(AON_WDOGCOUNT, 0u);
    reg_write(AON_WDOGKEY, WDOGKEY_VALUE);
    reg_write(AON_WDOGCMP0, WDOG_TIMEOUT_TICKS);
    reg_write(AON_WDOGKEY, WDOGKEY_VALUE);
    reg_write(AON_WDOGCFG, WDOGCFG_ENALWAYS | WDOGCFG_RSTEN | WDOGCFG_ZEROCMP);
}

void obc_recover_disarm_hardware(void)
{
    reg_write(AON_WDOGKEY, WDOGKEY_VALUE);
    reg_write(AON_WDOGCFG, 0u);
    reg_write(AON_WDOGKEY, WDOGKEY_VALUE);
    reg_write(AON_WDOGCOUNT, 0u);
}

void obc_recover_feed_hardware(void)
{
    reg_write(AON_WDOGKEY, WDOGKEY_VALUE);
    reg_write(AON_WDOGCOUNT, 0u);
}

static uint32_t boot_checksum(void)
{
    return OBC_BOOT_CHECKSUM_SEED ^ obc_boot_record.short_boots
           ^ (obc_boot_record.in_progress * 0x9E3779B9u);
}

static int boot_record_is_intact(void)
{
    return obc_boot_record.magic == OBC_BOOT_MAGIC
           && obc_boot_record.checksum == boot_checksum();
}

uint32_t obc_recover_short_boots(void)
{
    /*
     * A record that is not intact is not a count. At cold boot this region
     * holds whatever the RAM held, which on silicon is noise, and believing it
     * would put the system into safe mode on the strength of a random word.
     */
    return boot_record_is_intact() ? obc_boot_record.short_boots : 0u;
}

static void boot_record_write(uint32_t count, uint32_t in_progress)
{
    /* Payload, checksum, magic — the commit order used everywhere here, so a
     * reset mid-write leaves nothing that reads as authentic. */
    obc_boot_record.magic = 0u;
    obc_boot_record.short_boots = count;
    obc_boot_record.in_progress = in_progress;
    obc_boot_record.checksum = boot_checksum();
    obc_boot_record.magic = OBC_BOOT_MAGIC;
}

uint32_t obc_recover_boot_begins(void)
{
    uint32_t count = 0u;

    /*
     * A flag still raised means the previous boot never reached the point where
     * it would have lowered it — whether it faulted, reset itself, or was reset
     * in hardware by the AON watchdog with no software running at all. That
     * last case is why this is a flag and not an increment on the reset path.
     */
    if (boot_record_is_intact()) {
        count = obc_boot_record.short_boots;
        if (obc_boot_record.in_progress != 0u) {
            count++;
        }
    }

    boot_record_write(count, 1u);

    return count >= OBC_SHORT_BOOT_LIMIT ? OBC_RUNG_RESET_MACHINE : OBC_RUNG_NONE;
}

int obc_recover_boot_is_healthy(uint32_t frames_completed)
{
    /*
     * Evidence of a working executive, not of a completed initialisation. A
     * boot that got this far has turned the frame loop often enough for every
     * task in the table to have been dispatched at least once.
     *
     * The window is *consecutive* short boots, so one boot that genuinely works
     * ends the streak.
     */
    if (frames_completed < OBC_HEALTHY_FRAMES) {
        return 0;
    }
    boot_record_write(0u, 0u);
    return 1;
}

int obc_recover_is_suspended(uint32_t task_index, uint32_t frame)
{
    return s_suspended_task == task_index && s_suspended_frame == frame;
}

static void log_suspension(uint32_t task_index, uint32_t frame, uint32_t rung)
{
    if (obc_suspension_count >= OBC_SUSPEND_LOG_LEN) {
        /* Counted, never wrapped. A wrapped log would silently change what the
         * conformance checker compares against, turning an overflowed window
         * into one that merely looks different. */
        obc_suspension_overflow++;
        return;
    }
    obc_suspensions[obc_suspension_count].task = (uint8_t)task_index;
    obc_suspensions[obc_suspension_count].frame = (uint8_t)frame;
    obc_suspensions[obc_suspension_count].rung = (uint8_t)rung;
    obc_suspensions[obc_suspension_count].pad = 0u;
    obc_suspension_count++;
}

void obc_recover_escalate(uint32_t rung, uint32_t task_index, uint32_t frame)
{
    if (rung == OBC_RUNG_SUSPEND_TASK) {
        /*
         * Suspended for the task's next *due* frame, not for the next frame.
         *
         * The first version used frame + 1, which withholds nothing when the
         * task is not due then: a period-2 task suspended in frame 1 was never
         * going to run in frame 1, so the announcement had no gap behind it and
         * the suspension suspended nothing. Found by the checker's second
         * direction — an announcement with no gap is as much a defect as a gap
         * with no announcement, and it is the half that gets forgotten because
         * the exemption is written while thinking about false reds.
         */
        uint32_t period = obc_task_table[task_index].period_frames;
        uint32_t next_due = frame + (period == 0u ? 1u : period);

        s_suspended_task = task_index;
        s_suspended_frame = next_due;
        log_suspension(task_index, next_due, rung);
        return;
    }

    if (rung != OBC_RUNG_RESET_MACHINE) {
        return; /* rung 2 is empty; nothing selects it */
    }

    /*
     * The count is incremented here, on the way out, and not on the next boot.
     * A boot that dies before reaching any counting code would never be
     * counted, and a system dying earlier each time is precisely the loop this
     * protects against.
     */
    /*
     * The count is not touched here. The in-progress flag raised at boot is
     * still raised, so the next boot counts this one — which is what makes a
     * hardware reset count identically to a software one.
     */
    if (obc_recover_short_boots() >= OBC_SHORT_BOOT_LIMIT) {
        /*
         * Top of the ladder. Resetting has failed this many times running and
         * will not work once more; continuing costs the only thing left, which
         * is being observable.
         */
        obc_mode_enter_safe(OBC_SAFE_RESET_LOOP);
        return;
    }

    OBC_IGNORE(obc_uart_puts("recover: rung 3, resetting\r\n"));
    obc_reset_now();
}
