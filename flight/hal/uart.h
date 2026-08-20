/*
 * SiFive UART, transmit only.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OBC_HAL_UART_H
#define OBC_HAL_UART_H

#include <stdint.h>

#include "core/status.h"

/* Enables the transmitter. Must be called before any write. */
void obc_uart_init(void);

/* Writes one byte. Returns OBC_ERR_TIMEOUT if the transmit FIFO stayed full
 * for longer than the bounded retry limit; the byte is then dropped. */
obc_status_t obc_uart_putc(char c);

/* Writes a NUL-terminated string. Stops and returns the first error.
 * Returns OBC_ERR_INVALID if s is NULL. */
obc_status_t obc_uart_puts(const char *s);

/* Writes v as unsigned decimal, no padding. */
obc_status_t obc_uart_put_u32(uint32_t v);

/* Writes v as eight uppercase hex digits, prefixed with "0x". */
obc_status_t obc_uart_put_hex32(uint32_t v);

#endif /* OBC_HAL_UART_H */
