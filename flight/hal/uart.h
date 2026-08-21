/*
 * SiFive UART, transmit only. Two ports: a text console and a binary downlink.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OBC_HAL_UART_H
#define OBC_HAL_UART_H

#include <stdint.h>

#include "core/status.h"

/* Enables both transmitters. Must be called before any write. */
void obc_uart_init(void);

/* Writes one byte. Returns OBC_ERR_TIMEOUT if the transmit FIFO stayed full
 * for longer than the bounded retry limit; the byte is then dropped. */
OBC_MUST_CHECK obc_status_t obc_uart_putc(char c);

/* Writes a NUL-terminated string. Stops and returns the first error.
 * Returns OBC_ERR_INVALID if s is NULL. */
OBC_MUST_CHECK obc_status_t obc_uart_puts(const char *s);

/* Writes v as unsigned decimal, no padding. */
OBC_MUST_CHECK obc_status_t obc_uart_put_u32(uint32_t v);

/* Writes v as eight uppercase hex digits, prefixed with "0x". */
OBC_MUST_CHECK obc_status_t obc_uart_put_hex32(uint32_t v);

/*
 * Writes raw bytes to the downlink port, which carries telemetry and nothing
 * else.
 *
 * Bytes rather than a string: a frame contains NUL and every other value, so a
 * NUL-terminated write would truncate it. Separate from the console for the
 * reason given in uart.c — the console is a development artefact and the
 * downlink is the system's product, and a single line would need a
 * de-interleaver that would end up in flight code.
 *
 * Stops at the first refusal and returns it. The caller is not allowed to assume
 * the whole frame went out.
 */
OBC_MUST_CHECK obc_status_t obc_uart_downlink_write(const volatile uint8_t *bytes,
                                                    uint32_t len);

#endif /* OBC_HAL_UART_H */
