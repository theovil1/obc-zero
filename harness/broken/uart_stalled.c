/*
 * DELIBERATELY MODIFIED. Not flight code. Never linked into a flight image.
 *
 * A copy of flight/hal/uart.c whose downlink reads its transmit status from a
 * word the harness owns instead of from the device register.
 *
 * **Why a substitute and not a register injection.** QEMU's sifive_uart does
 * model an eight-entry FIFO and does set the full bit — but it drains on a
 * bottom half that runs the moment the vCPU resumes, so a bit set from the
 * debugger is clear again after a single retired instruction. Measured with
 * `stepi`; recorded in ADR 0009, fact 3.
 *
 * **Why the source is chosen outside the loop, and it is the whole point.** The
 * first version of this stub tested the stall with a countdown *inside* the poll
 * loop. That added about five instructions to a loop the flight build runs in
 * four, so a stalled dispatch measured 3949 instructions instead of the 2528 the
 * flight build pays — over its 3000 budget. The task overran, climbed the
 * ladder, and reset the machine: the instrument pushed the system past the very
 * threshold the test existed to prove it stayed under, and the test reported
 * PASS because it never checked whether the system survived.
 *
 * Selecting the pointer once, before the loop, leaves the loop body identical to
 * the flight build's — the same `lw`, the same branch, the same allowance
 * arithmetic. A stalled dispatch then costs what a stalled dispatch costs.
 *
 * That also makes the poll cost measurable: with the port held full, the
 * dispatch rises above nominal by exactly the allowance times the per-poll cost,
 * which is the figure flight/core/tasks.c asserts against. The constant feeding a
 * compile-time proof is checked by a run rather than believed.
 *
 * SiFive UART, transmit only. Two ports, and they carry different things.
 *
 * Both base addresses come from the QEMU machine model
 * (emu/sifive_e-mtree.txt, two `riscv.sifive.uart` regions). UART1 was verified
 * to be genuinely modelled and not merely present in the address map, by writing
 * a byte through the debugger and reading it back from the host's capture — a
 * device that accepted writes into nothing would have produced a green test on a
 * frame that was never emitted.
 *
 * **The console and the downlink are separate ports on purpose**, and the reason
 * is not that mixing them upset a grep. They are different things: the console
 * exists because somebody is developing, and the downlink is the system's
 * product. Sharing one line would mean writing a de-interleaver, and that
 * de-interleaver would end up in flight code. Keeping them apart also fixes the
 * boundary M7's command uplink needs, which does not arrive on a debug console
 * either.
 *
 * Polled by design. Telemetry has to keep coming out after an injected fault
 * has wrecked the interrupt path, so the transmit path must not depend on it.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hal/uart.h"

#include <stdint.h>

#include "core/status.h"

/* Register windows, eight 32-bit registers each. */
#define UART0_BASE 0x10013000u /* console: banner, announcements, diagnostics */
#define UART1_BASE 0x10023000u /* downlink: telemetry frames, bytes only */

#define UART_TXDATA 0x00u /* [31] full, [7:0] data */
#define UART_RXDATA 0x04u
#define UART_TXCTRL 0x08u /* [0] txen, [18:16] txcnt watermark */
#define UART_RXCTRL 0x0Cu
#define UART_IE 0x10u
#define UART_IP 0x14u
#define UART_DIV 0x18u

#define UART_TXDATA_FULL 0x80000000u
#define UART_RXDATA_EMPTY 0x80000000u
#define UART_TXCTRL_TXEN 0x00000001u
#define UART_RXCTRL_RXEN 0x00000001u

/*
 * Bounded retry limit, now derived rather than chosen. See OBC_UART_TX_POLL_INSTR
 * and OBC_UART_TX_RETRY_TOTAL in the header, and ADR 0009 for why the previous
 * figure — 100000, per byte — was a defect and not a conservative margin.
 *
 * No loop in flight code may be unbounded. That rule was satisfied by the old
 * constant and it was not enough: a bound unrelated to the budget that governs
 * the loop is a bound in name. **A loop inside a budgeted task is bounded by
 * that budget or it is not bounded in any way that matters.**
 */
#define UART_TX_RETRY_LIMIT OBC_UART_TX_RETRY_TOTAL

/*
 * Baud divisor. QEMU's sifive_uart ignores this entirely, since the chardev
 * backend has no baud rate, but leaving it unset would be a latent defect the
 * day this runs on real silicon. 16 MHz core clock, 115200 baud.
 */
#define UART_DIV_115200 138u

/* Named separately so the assertion in tasks.c and the code here cannot drift:
 * both spell the same constant. */
#define UART_TX_RETRY_TOTAL_CHECKED OBC_UART_TX_RETRY_TOTAL

static inline void reg_write(uint32_t base, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)(base + offset) = value;
}

static inline uint32_t reg_read(uint32_t base, uint32_t offset)
{
    return *(volatile uint32_t *)(uintptr_t)(base + offset);
}

static void port_init(uint32_t base)
{
    reg_write(base, UART_IE, 0u); /* polled, no interrupts */
    reg_write(base, UART_DIV, UART_DIV_115200);
    reg_write(base, UART_TXCTRL, UART_TXCTRL_TXEN);
}

/*
 * Writes one byte, spending at most *allowance polls and decrementing it by what
 * it spent.
 *
 * The allowance is passed in and carried out so a caller can hand one budget to
 * a whole sequence of bytes. That is the difference between a frame costing at
 * most one allowance and costing forty-five of them.
 */
/*
 * The downlink's status, when the harness is holding it. Set to
 * UART_TXDATA_FULL to refuse, 0 to let go. .noinit so a reset does not silently
 * release a stall a test is relying on.
 */
volatile uint32_t obc_uart_stall_status __attribute__((section(".noinit")));
volatile uint32_t obc_uart_stall_active __attribute__((section(".noinit")));

/*
 * The allowance, as a variable the harness can set.
 *
 * This is what makes the per-poll cost measurable *exactly* rather than bounded.
 * Two runs of this image with allowances A and 2A differ by precisely A polls —
 * everything else about the dispatch is identical — so the cost of one poll is
 * the difference divided by A, and it can be held against the constant the
 * flight build asserts with.
 *
 * Without it the poll cost is only checked by a floor, and a floor fails in one
 * direction: a figure that is too *low* lowers the floor and makes the test
 * easier to pass. That is the unsafe direction, and it is the direction a stale
 * constant drifts in when code gets cheaper.
 *
 * Only in this substitute. The flight build's allowance stays a compile-time
 * constant the static assertion can reason about.
 */
volatile uint32_t obc_uart_stall_allowance __attribute__((section(".noinit")));

static obc_status_t port_putc(uint32_t base, uint8_t byte, uint32_t *allowance)
{
    /* STUB: chosen once, outside the loop, so the loop below is instruction for
     * instruction the flight build's. */
    const volatile uint32_t *status =
        (base == UART1_BASE && obc_uart_stall_active != 0u)
            ? &obc_uart_stall_status
            : (const volatile uint32_t *)(uintptr_t)(base + UART_TXDATA);

    for (;;) {
        if ((*status & UART_TXDATA_FULL) == 0u) {
            reg_write(base, UART_TXDATA, (uint32_t)byte);
            return OBC_OK;
        }
        if (*allowance == 0u) {
            return OBC_ERR_TIMEOUT;
        }
        (*allowance)--;
    }
}

/*
 * The allowance as a symbol, so the harness can choose "just under" and "just
 * over" from the figure the vehicle actually applies rather than from a copy.
 * An injector is a program and its thresholds are part of it: a stall shorter
 * than the allowance proves the retries are spent, and it only proves that if it
 * is measured against the real allowance.
 *
 * `retain` because nothing in flight code reads it and --gc-sections is right to
 * drop what only the ground wants.
 */
__attribute__((used, retain)) const uint32_t obc_uart_tx_retry_total =
    OBC_UART_TX_RETRY_TOTAL;

/* The per-poll cost, exported for the same reason: the harness predicts what a
 * stalled dispatch must cost, and a prediction built from its own copy of this
 * number would keep agreeing with itself after the figure moved. */
__attribute__((used, retain)) const uint32_t obc_uart_tx_poll_instr =
    OBC_UART_TX_POLL_INSTR;

__attribute__((used, retain)) const uint32_t obc_uart_tx_byte_instr =
    OBC_UART_TX_BYTE_INSTR;

void obc_uart_init(void)
{
    /*
     * Both, here. A downlink brought up lazily on its first frame would be a
     * port whose initialisation only runs in the cases where telemetry already
     * works, and the first frame after a fault is exactly when that is not true.
     */
    port_init(UART0_BASE);
    port_init(UART1_BASE);
    /* The uplink's receiver, enabled here and nowhere else. A receiver brought
     * up lazily on the first poll is one whose initialisation only runs in the
     * cases where the link already works. */
    reg_write(UART1_BASE, UART_RXCTRL, UART_RXCTRL_RXEN);
}

obc_status_t obc_uart_uplink_getc(uint8_t *out)
{
    uint32_t rx;

    if (out == 0) {
        return OBC_ERR_INVALID;
    }

    /* One read, no retry, no wait. The empty bit and the data share a register:
     * reading it consumes a byte when one is there, so this must not be read
     * twice. */
    rx = reg_read(UART1_BASE, UART_RXDATA);
    if ((rx & UART_RXDATA_EMPTY) != 0u) {
        return OBC_ERR_TIMEOUT;
    }
    *out = (uint8_t)(rx & 0xFFu);
    return OBC_OK;
}

obc_status_t obc_uart_putc(char c)
{
    /*
     * The console gets the allowance per byte, because its writes are not a
     * single object the way a frame is: a banner truncated halfway is still
     * readable, and there is no sum for a host to reject.
     *
     * It gets the *derived* figure rather than the old one for the reason ADR
     * 0009 gives: the escalation ladder announces itself through here, from
     * inside a dispatch, which is exactly the position telemetry was in. Fixing
     * one of the two would have been fixing the instance rather than the defect.
     */
    uint32_t allowance = UART_TX_RETRY_LIMIT;

    return port_putc(UART0_BASE, (uint8_t)c, &allowance);
}

obc_status_t obc_uart_downlink_write(const volatile uint8_t *bytes, uint32_t len)
{
    uint32_t i;

    if (bytes == 0) {
        return OBC_ERR_INVALID;
    }

    /*
     * One allowance, shared by every byte, and the call abandons the frame the
     * moment it runs out. A partial frame fails its sum on the host and is
     * dropped there, which is the correct outcome for a congested downlink.
     *
     * The caller is told, and is expected to count it: a frame that was not sent
     * and a frame that never came due look identical from the ground, and they
     * need different responses.
     */
    {
        uint32_t allowance = (obc_uart_stall_active != 0u
                              && obc_uart_stall_allowance != 0u)
                                 ? obc_uart_stall_allowance
                                 : UART_TX_RETRY_TOTAL_CHECKED;

        for (i = 0u; i < len; i++) {
            obc_status_t st = port_putc(UART1_BASE, bytes[i], &allowance);

            if (st != OBC_OK) {
                return st;
            }
        }
    }
    return OBC_OK;
}

obc_status_t obc_uart_puts(const char *s)
{
    obc_status_t st;
    uint32_t i;

    if (s == 0) {
        return OBC_ERR_INVALID;
    }

    /* Bounded: a string longer than this is a corrupted pointer, not a
     * message. The bound is the loop's proof of termination. */
    for (i = 0u; i < 4096u; i++) {
        if (s[i] == '\0') {
            return OBC_OK;
        }
        st = obc_uart_putc(s[i]);
        if (st != OBC_OK) {
            return st;
        }
    }

    return OBC_ERR_INVALID;
}

obc_status_t obc_uart_put_u32(uint32_t v)
{
    char buf[10];
    uint32_t n = 0u;
    obc_status_t st;

    if (v == 0u) {
        return obc_uart_putc('0');
    }

    /* Bounded: 2^32-1 is ten decimal digits. */
    while (v > 0u && n < 10u) {
        buf[n] = (char)('0' + (v % 10u));
        v /= 10u;
        n++;
    }

    while (n > 0u) {
        n--;
        st = obc_uart_putc(buf[n]);
        if (st != OBC_OK) {
            return st;
        }
    }

    return OBC_OK;
}

obc_status_t obc_uart_put_hex32(uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    obc_status_t st;
    int32_t shift;

    st = obc_uart_puts("0x");
    if (st != OBC_OK) {
        return st;
    }

    for (shift = 28; shift >= 0; shift -= 4) {
        st = obc_uart_putc(digits[(v >> (uint32_t)shift) & 0xFu]);
        if (st != OBC_OK) {
            return st;
        }
    }

    return OBC_OK;
}
