# Where the emulator is kinder than the silicon

Every entry here is a place where QEMU `sifive_e` behaves better than an FE310
will, so that a green result on this machine is **weaker evidence than it looks**.

This is not a list of things QEMU does not model. It does not model radiation or
thermal behaviour either, and nobody is going to be surprised by that. It is the
narrower and more dangerous list: **behaviours the code depends on, which the
emulator supplies for free and the hardware will not.** Each one is a test that
passes here and a defect that ships.

It is kept in one file rather than scattered through the records that discovered
it, because its value is as a whole. **When the board arrives, this is the list
of what breaks**, and a list assembled from eight ADRs on the afternoon it is
needed is a list with holes in it.

## Status at a glance

**The column that matters is the last one.** At twenty entries nobody will read
the prose to find out which gaps are still risks, and a list where that cannot be
seen at a glance is a list that stops being consulted.

| # | Gap | Status |
|---|---|---|
| 1 | RAM comes up zeroed | **Covered** — `make test-poisoned` |
| 2 | The UART never refuses a byte | **Partial** — policy covered by `make test-uart-stall`; duration open |
| 3 | Instruction timing has no memory system | **Open** — nothing compensates; every budget must be re-derived |

Three states and no others:

- **Covered** — a test on this machine produces the hardware's behaviour, and it
  is named. The gap is closed and the entry stays, because the entry is the only
  record of why that test exists.
- **Partial** — part of the behaviour is reproduced and part is not. The entry
  says which part, in those words. "Partial" without that sentence is "open".
- **Open** — nothing compensates. This is an allowed and useful answer, and it is
  better than a compensation invented to fill the column.

## How to read an entry

| Field | Meaning |
|---|---|
| **Status** | Covered, Partial or Open — and the table above must agree |
| **Found** | The milestone and commit where the gap became visible |
| **The emulator** | What QEMU does |
| **The hardware** | What the FE310 will do instead |
| **What passes here and should not** | The specific green result that is weak |
| **What the port must do** | The concrete change, not a direction |
| **Covered by** | Whatever compensates for it today, or "nothing" |

---

## 1. RAM comes up zeroed

- **Status:** **Covered** — `make test-poisoned`
- **Found:** M1, `3fdb632`
- **The emulator:** QEMU zeroes DTIM at reset. Every uninitialised word reads 0.
- **The hardware:** SRAM at power-on holds whatever the cells settled into —
  neither zero nor stable between boots, and *not* cleared by a warm reset.
- **What passes here and should not:** every check of a `.noinit` record. A magic
  word, a checksum, a boot-in-progress flag: on this machine a cold boot hands
  each of them a clean zero, so a validator that accepts zero, or one that never
  runs because the magic happens to be absent, looks correct.
- **What the port must do:** nothing, if the compensation below holds. This is the
  one gap already closed.
- **Covered by:** `make test-poisoned` boots on RAM filled with a non-zero
  pattern through the debugger, so every record validator meets a cold boot the
  emulator cannot produce. The compensation was built at M1 for exactly this
  reason and it is the model for the rest of this file.

---

## 2. The UART never refuses a byte

- **Status:** **Partial** — the policy is covered by `make test-uart-stall`; the
  *duration* is not reproduced by anything and is the part the hardware changes
- **Found:** M6, and closed as far as it can be at `624ec76`
- **The emulator:** the chardev accepts every byte the instant it is offered.
  `sifive_uart` does model an eight-entry transmit FIFO and does set the full
  bit — but it drains on a bottom half that runs the moment the vCPU resumes, so
  a stall held from the debugger is **gone after one retired instruction**
  (measured with `stepi`, ADR 0009 fact 3).
- **The hardware:** at 115200 baud one byte occupies the line for 1356 core
  instructions. A 45-byte frame is **61035 instructions**, 12.5 % of a whole
  31.25 ms frame, against a declared telemetry budget of 3000.
- **What passes here and should not:** the telemetry task's measured cost of
  1466 instructions. It does not measure telemetry. It measures a UART with no
  baud rate, and every budget claim resting on it is a claim about a machine
  where transmission is free.
- **What the port must do:** **not raise the budget.** Twenty times is not a
  budget that is too small, it is the wrong shape. The transmit must become
  non-blocking: the frame is handed to a driver that drains it across frames,
  and the dispatch stops containing the wire time. ADR 0009, decision 4.
- **Covered by:** partially. `harness/broken/uart_stalled.c` proves the *policy*
  survives a refusal — the frame is shed, counted, announced, and the machine
  does not reset. It proves nothing about the *duration*, which is the part the
  hardware changes.

---

## 3. Instruction timing has no memory system in it

- **Status:** **Open** — nothing compensates
- **Found:** M1, ADR 0002; restated here because it underlies both entries above
- **The emulator:** `-icount shift=6` retires one instruction per fixed tick
  quantum. No cache, no flash wait states, no bus contention.
- **The hardware:** the FE310 executes from SPI flash through a cache. A miss
  costs tens of cycles, and the cost depends on the access pattern rather than on
  the instruction count.
- **What passes here and should not:** every budget in `flight/core/tasks.c`.
  They are expressed in retired instructions precisely so they are portable in
  *form*, and their values are calibrated against a machine where an instruction
  is an instruction.
- **What the port must do:** re-derive every budget on hardware, and expect the
  ratio to differ per task rather than uniformly — a task with a tight loop and
  one that walks a table will not scale by the same factor.
- **Covered by:** the form. Budgets are in `minstret`, which exists on silicon,
  so the measurement transfers even though the number does not. ADR 0002 requires
  the recalibration; this file records what it will cost.

---

## Rules for this file

1. **An entry is added by the milestone that discovers the gap, not later.** The
   discovery is the expensive part and it is perishable.
2. **"What the port must do" is a change, not a direction.** "Revisit the
   timing" is not an entry; "re-derive every budget and expect per-task ratios"
   is.
3. **Open is an allowed answer** and is more useful than a compensation invented
   to fill the column. Entry 2 is Partial and says which part, in the status line
   itself rather than in the prose below it.
4. **The summary table and the entries must agree.** Two places to state a status
   is one place too many, and the table is the one people will read — so an entry
   whose status changes changes both, in the same commit.
5. **A closed gap stays in the file.** Entry 1 is closed; removing it would lose
   the reason `test-poisoned` exists, and the next person to find that test
   expensive would delete it.

## What this file is not

It is not a portability checklist for the code. Endianness, register widths and
alignment are handled by the toolchain and the language, and nothing here is
about them.

It is about **evidence**. Every line in `docs/reports/` was produced on this
machine. This file is the standing note of how much less those lines mean than
they appear to, and it should be read beside them rather than after them.
