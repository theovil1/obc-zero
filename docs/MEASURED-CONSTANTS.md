# Constants that encode a measurement

**A constant that encodes a measurement is only a control while something
re-measures it.**

This file exists because of a defect that none of the previous seven resembled.
The compile-time assertion containing the telemetry budget was real, it passed,
and it guarded nothing — because its input, `T1_NOMINAL_INSTR`, was a figure
measured on a 47-byte frame that had since become 91 bytes. Not an absent
control, not one at the wrong layer, not one nobody tested. **A working control
whose input had quietly stopped being true.**

Every constant below is in that position by default. A constant that is
*derived* — computed from other constants by a rule — cannot drift, because
changing an input changes it. A constant that is *measured* drifts silently the
moment the thing it measured changes.

## The register

| Constant | Value | Measures | Re-checked by | Status |
|---|---:|---|---|---|
| `T1_NOMINAL_INSTR` | 2585 | a telemetry dispatch with the downlink accepting | `make test`, exactly | **Watched** |
| `OBC_MILLI_INSTR_PER_TICK` | 476837 | instructions retired per machine-timer tick | the boot's own timebase check, with a tolerance | **Watched** |
| `OBC_UART_TX_POLL_INSTR` | 5 | one poll of a full transmit FIFO | `make test-uart-stall`, two runs whose allowances differ | **Watched** |
| `OBC_UART_TX_BYTE_INSTR` | 7 | one successful byte write | the same test's floor, as a **lower bound** | **Partial** |
| `T0`–`T4_BUDGET` | 1000–12000 | headroom over a measured worst dispatch | the executive's overrun count, per run | **Watched** |
| `UART_DIV_115200` | 138 | — | — | **Derived**, not measured |

`UART_DIV_115200` is in the table to be ruled out: it is 16 MHz over 115200, a
division rather than an observation, and it cannot go stale without its inputs
going stale first.

## What each status means

**Watched.** Something compares the constant against the thing it measures, and
refuses on a mismatch. This is the only status that makes a measured constant
safe to depend on.

**Partial.** Something would notice a change in one direction and not the other.
`OBC_UART_TX_BYTE_INSTR` feeds a *floor* in `stall_check.py`, and a floor fails
only when the constant is too high: a figure that is too **low** lowers the floor
and makes the test easier to pass. That is the unsafe direction, and it is the
one a stale figure drifts in when code gets cheaper.

`OBC_UART_TX_POLL_INSTR` was Partial for the same reason and is not any more. The
substitute takes its allowance from a word the harness sets, so two runs of one
image differ by a known number of polls **and by nothing else**; the difference
divided by that number is the cost of one poll. Bracketed rather than divided,
because the runs also differ by a fixed instruction or two of loop setup — the
residual must stay under one poll, which pins the figure to an exact integer
without inventing a margin. Verified in both directions: 4 and 6 are both
refused, by different checks.

**Unwatched.** Nothing re-measures it. There are none today and the point of this
file is that there should not be — a new measured constant arrives with its
re-check or it arrives as a defect waiting for the thing it measures to move.

## Re-deriving each one

| Constant | How |
|---|---|
| `T1_NOMINAL_INSTR` | `make test`, read `task telemetry max=` |
| `OBC_MILLI_INSTR_PER_TICK` | `make measure`, read the `base :` line |
| `OBC_UART_TX_POLL_INSTR` | `make test-uart-stall` reports it; or disassemble `obc_uart_downlink_write` and count the poll loop |
| `OBC_UART_TX_BYTE_INSTR` | same disassembly, the successful-write path |
| `T*_BUDGET` | `make test`, read each `max=` and leave the headroom the task's own comment justifies |

Each is one command. **The cost of re-deriving is not why they go stale** — they
go stale because nothing asks.

## The rule for adding one

A new measured constant arrives with three things or it does not arrive:

1. **The measurement**, in a comment, with the command that produced it.
2. **Something that re-measures it** and refuses on a mismatch. Where an exact
   comparison is impossible, a bound is acceptable and the register records
   which direction it fails in.
3. **A row here.**

The second is the one that gets skipped, and skipping it is not visible: the
constant looks like every other constant, the assertion that uses it looks like
every other assertion, and the whole arrangement works until the day the measured
thing changes.

## Where this does not apply

Constants declared **arbitrary** are a different category and are not listed
here: `OBC_SHORT_BOOT_LIMIT`, `OBC_SENSOR_STUCK_LIMIT`, `OBC_CMD_ARM_FRAMES`,
`OBC_CMD_QUEUE_LEN`, `OBC_UART_TX_RETRY_TOTAL`. Each says in its own comment that
it is not derived from anything, and each is a figure to calibrate against a
mission that does not exist.

**An arbitrary constant cannot go stale, because it never encoded a fact.** That
is the whole difference, and it is why declaring one arbitrary is a real act
rather than an apology.
