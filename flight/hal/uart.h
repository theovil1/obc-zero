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

/*
 * The cost of one poll of a full transmit FIFO, in retired instructions.
 *
 * Measured from the disassembly of the shipped binary, not estimated: the loop
 * is `lw` / `bltz` / `beqz` / `addi` / `j`, five instructions, and it is the same
 * five on both ports.
 *
 * **It was four until the allowance became one budget for the whole call.** The
 * per-byte version kept its counter in a register the loop already held; a
 * shared allowance adds the exhaustion test to the loop body. Written down
 * because the figure was carried over from the earlier disassembly and was
 * wrong by one for exactly as long as nobody re-derived it — which is the same
 * failure as the constant it replaced, one order of magnitude smaller.
 *
 * Re-derive it with
 *
 *   riscv64-unknown-elf-objdump -d build/obc.elf
 *
 * if the compiler or its flags ever change. A figure used in a compile-time
 * assertion has to be checkable, or the assertion is checking a belief.
 */
#define OBC_UART_TX_POLL_INSTR 5u

/*
 * How long a transmit may wait, in polls.
 *
 * **This replaces a constant of 100000 that nobody derived from anything**, and
 * which was spent *per byte* — so one stalled byte cost up to 400000
 * instructions inside a task budgeted 3000, in a frame that holds 487424. See
 * docs/adr/0009-emission-under-refusal.md.
 *
 * The figure comes from the budget of the task that does the emitting, minus its
 * measured nominal cost, divided by the poll cost above. flight/core/tasks.c
 * asserts that the three still fit; lower the budget or raise this and the build
 * fails rather than the vehicle.
 *
 * **It is deliberately shorter than one byte time on real hardware** — 339 polls
 * at 115200 baud — which means a genuinely busy port drops the frame instead of
 * waiting for it. That is the intended answer, not an oversight: telemetry sheds
 * data rather than the system. ADR 0009, decision 1.
 */
#define OBC_UART_TX_RETRY_TOTAL 256u

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
 * **One retry allowance for the whole call**, not one per byte. A per-byte bound
 * makes the worst case the frame length times the bound, which is how a
 * 45-byte frame came to be able to cost 82 % of a frame every other task shares.
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
