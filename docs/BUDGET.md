# RAM budget

The `sifive_e` target has **16384 bytes of RAM** and nothing else. No heap, no
MMU, no external memory. This file allocates that budget between milestones
before they are written, so that the first subsystem to arrive does not simply
take what it wants and leave the later ones to fail at link time.

An allocation here is a ceiling, not a reservation. Unused bytes stay available
to the reserve, but exceeding a line requires changing this file and saying why.

- **Established:** 2026-08-20, at the end of M0
- **Authority:** the link script asserts on total RAM overflow, so exceeding the
  budget overall is a build failure. Per-line enforcement is by review.

## Allocation

| Line | Milestone | Bytes | Share | Status |
|---|---|---:|---:|---|
| Stack | M0 | 1024 | 6.3 % | **Allocated**, 64 B peak measured |
| `.data` + `.bss`, core | M0–M3 | 0 | 0.0 % | **In use**, currently empty |
| Scheduler state and task table | M2 | 512 | 3.1 % | **In use**, 384 B measured |
| Panic context and safe-mode state | M3 | 256 | 1.6 % | Planned |
| Triple-redundant critical state | M4 | 3072 | 18.8 % | Planned |
| Telemetry frames and sensor mocks | M6 | 2048 | 12.5 % | Planned |
| Command queue | M7 | 2048 | 12.5 % | Planned |
| Event log buffer | M8 | 4096 | 25.0 % | Planned |
| **Unallocated reserve** | — | **3328** | **20.3 %** | Held |
| **Total** | | **16384** | 100 % | |

## Reasoning per line

**Stack, 1024 B.** Measured, not guessed: the M0 boot path peaks at 64 bytes,
read back from the `0xDEADBEEF` paint pattern and reported on the banner every
boot. 1 KiB is sixteen times the measured peak. It has to absorb the trap frame
M1 introduces, which nests on top of whatever task was running, and the task
call depth M2 introduces. There is no recursion and no allocation anywhere in
the flight code, so stack growth is bounded by call-graph depth alone and can be
checked statically. Revisit at M2 with a measurement taken under scheduler load.

**Scheduler, 384 B of 512 B.** Measured, not estimated.

The figures in this file are the **flight** figures: nothing measured here is
compiled out of the image intended to fly. That was settled in ADR 0004 rather
than assumed — gating the trace behind a build flag would have made every budget
below a claim about a binary that never flies, so no second configuration is
tracked and none is needed.

The trace buffer is 256 B of it — one byte of task index per dispatch, sized to hold the 16-frame
assertion window with room to spare. The task table itself costs nothing here:
it is `const` and lives in flash, which also means a corrupted RAM word cannot
turn a period into something else or redirect a function pointer. Only the
mutable per-task accounting is charged to RAM.

**Triple-redundant critical state, 3072 B — a ceiling, and one M4 does not
approach.** Three copies of up to 1 KiB. If 1 KiB of critical state ever proves
too tight, the answer is to reduce what counts as critical, not to drop to two
copies: two copies detect corruption but cannot vote on it.

What counts as critical is decided by the criterion in ADR 0006, written before
M4 rather than while the budget was refusing — otherwise "critical" comes to
mean "whatever fit". Applied to the state that exists, it admits **one live
variable, `obc_mode`, four bytes**. The rest of this line stays unspent and
returns to the reserve. The list is expected to grow as M5, M7 and M8 arrive
with state that must be argued against the criterion rather than assumed into
it.

**Event log, 4096 B.** The largest line overall. An event log that wraps too
quickly cannot explain an anomaly after the fact, which defeats its purpose. If
M8 shows this is more than the write path needs, the surplus returns to reserve.

**Telemetry, 2048 B.** Fixed-layout frames plus the mock sensor backend the
harness drives. Frames are compile-time defined, so this line is knowable
exactly at M6 rather than estimated.

**Command queue, 2048 B.** A statically sized, time-tagged queue. The size sets
the maximum number of pending commands, which is a mission parameter rather than
an implementation detail, so it should be revisited against a concrete scenario.

**Reserve, 3328 B.** Twenty percent, held back and assigned to nothing. This
exists because every estimate above except the stack is unmeasured, and because
the fault-tolerance work in M4 and M5 is exactly the kind that discovers a need
for memory late. Spending the reserve early would be the easiest way to make
this project fail at M8.

## Rules

1. A milestone that needs more than its line updates this file first, in the
   same change, stating what it took and from where.
2. The reserve is spent by explicit decision, never by drift.
3. Anything that can live in flash lives in flash. Constant tables, lookup
   tables, and string literals belong in `.rodata`, which costs nothing here.
   The flash budget is 4 MiB against 4134 bytes used, tracked in the Flash
   section below; RAM is the only scarce resource on this board.
4. No line may be met by making a buffer dynamic. There is no allocator and
   there will not be one.

## Current consumption

Measured on commit `4df8c5d` by `make measure`. See `docs/LOGBOOK.md` for the
run that produced these numbers.

| | Bytes |
|---|---:|
| `.data` | 0 |
| `.bss` | 4 |
| Stack reserved | 1024 |
| Stack peak observed | 64 |
| `.noinit` (fault record) | 36 |
| Scheduler state and trace | 384 |
| **Total committed** | **1448 of 16384** |

Remaining unallocated after the planned lines above: 3328 B.

## Flash

Flash is not scarce here — 4 MiB against a four-figure image — and nothing in
this section asks for it to be optimised. It exists so that growth stays an
observed fact rather than a discovery.

| | Bytes | Note |
|---|---:|---|
| M0, first boot | 874 | banner and UART only |
| M1, timer and traps | 2698 | tripled on one milestone |
| M2, scheduler | 4134 | |
| **Alert threshold** | **262144** | **arbitrary, and declared so** |
| Budget | 4194304 | the link script's FLASH region |

The alert threshold is one sixteenth of the region. It is not derived from
anything: it is a round number chosen so that crossing it forces a conversation
rather than passing unnoticed. Revise it with a reason, not with a shrug.

M8 must either absorb the execution trace into the event log or explain why two
independent records of what recently happened both deserve space; 16 KiB cannot
afford the duplication. Recorded in ADR 0004 as a commitment, not a preference.

M6, M7 and M8 each add tables and fixed-layout frames, all of which belong in
`.rodata` and therefore in flash. Rule 3 below actively pushes data here, so the
line will move. Tracking it costs one row.

## Enforcement

A figure in this file is a claim, and a claim is worth nothing if the number can
move without anyone noticing. Per-section sizes are therefore pinned in
`docs/size-reference.txt` and compared on every `make measure`. Any drift fails.

This covers flash and RAM together, and it replaces the ad-hoc checking that
would otherwise have to be remembered. Changing a size is allowed and expected;
doing it silently is not. The sequence is `make size-accept`, then explain the
change in the commit message.

The guard was validated by making it fail before it was trusted: a one-character
edit to a banner string moved `.rodata` by 4 bytes and was caught. It found a
real defect on its first run, described in `docs/LOGBOOK.md`.
