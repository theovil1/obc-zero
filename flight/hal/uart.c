/*
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
#define UART_TXCTRL_TXEN 0x00000001u

/*
 * Bounded retry limit for a full transmit FIFO. No loop in the flight code may
 * be unbounded, and a UART that never drains is a plausible fault-injection
 * outcome: dropping a character is always preferable to hanging the core.
 */
#define UART_TX_RETRY_LIMIT 100000u

/*
 * Baud divisor. QEMU's sifive_uart ignores this entirely, since the chardev
 * backend has no baud rate, but leaving it unset would be a latent defect the
 * day this runs on real silicon. 16 MHz core clock, 115200 baud.
 */
#define UART_DIV_115200 138u

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

static obc_status_t port_putc(uint32_t base, uint8_t byte)
{
    uint32_t retries;

    for (retries = 0u; retries < UART_TX_RETRY_LIMIT; retries++) {
        if ((reg_read(base, UART_TXDATA) & UART_TXDATA_FULL) == 0u) {
            reg_write(base, UART_TXDATA, (uint32_t)byte);
            return OBC_OK;
        }
    }

    return OBC_ERR_TIMEOUT;
}

void obc_uart_init(void)
{
    /*
     * Both, here. A downlink brought up lazily on its first frame would be a
     * port whose initialisation only runs in the cases where telemetry already
     * works, and the first frame after a fault is exactly when that is not true.
     */
    port_init(UART0_BASE);
    port_init(UART1_BASE);
}

obc_status_t obc_uart_putc(char c)
{
    return port_putc(UART0_BASE, (uint8_t)c);
}

obc_status_t obc_uart_downlink_write(const volatile uint8_t *bytes, uint32_t len)
{
    uint32_t i;

    if (bytes == 0) {
        return OBC_ERR_INVALID;
    }

    /*
     * Abandons the frame on the first refusal rather than pushing the remaining
     * bytes at a port that just said it was full. A partial frame fails its sum
     * on the host and is dropped, which is the correct outcome for a congested
     * downlink; carrying on would multiply one stall by the frame length.
     */
    for (i = 0u; i < len; i++) {
        obc_status_t st = port_putc(UART1_BASE, bytes[i]);

        if (st != OBC_OK) {
            return st;
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
