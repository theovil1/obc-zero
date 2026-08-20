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
| Scheduler state and task table | M2 | 512 | 3.1 % | Planned |
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

**Triple-redundant critical state, 3072 B.** Three copies of a 1 KiB state
block. This is the single largest functional line, and deliberately so: it is
the mechanism the whole project exists to demonstrate. If 1 KiB of critical
state proves too tight, the answer is to reduce what counts as critical, not to
drop to two copies. Two copies detect corruption but cannot vote on it.

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
   The flash budget is 4 MiB against 858 bytes used; RAM is the only scarce
   resource on this board.
4. No line may be met by making a buffer dynamic. There is no allocator and
   there will not be one.

## Current consumption

Measured on commit `3ca4ce4` by `make measure`. See `docs/LOGBOOK.md` for the
run that produced these numbers.

| | Bytes |
|---|---:|
| `.data` | 0 |
| `.bss` | 0 |
| Stack reserved | 1024 |
| Stack peak observed | 64 |
| **Total committed** | **1024 of 16384** |

Remaining unallocated after the planned lines above: 3328 B.

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
