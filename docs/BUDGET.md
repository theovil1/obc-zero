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
| Stack | M0 | 1024 | 6.3 % | **Allocated**, 112 B peak measured |
| `.data` + `.bss`, core | M0–M3 | 0 | 0.0 % | **In use**, folded into the lines below |
| Scheduler state and task table | M2 | 512 | 3.1 % | **In use**, 384 B measured |
| Panic context and safe-mode state | M3 | 256 | 1.6 % | Planned |
| Triple-redundant critical state | M4 | 3072 | 18.8 % | **In use**, 344 B measured |
| Telemetry frames and sensor mocks | M6 | 2048 | 12.5 % | **In use**, 112 B measured |
| Command queue | M7 | 2048 | 12.5 % | **In use**, 224 B measured |
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
variable, `obc_mode`, four bytes**. The list is expected to grow as M5, M7 and
M8 arrive with state that must be argued against the criterion rather than
assumed into it.

**Measured: 344 B of the 3072.** Three copies of 8 bytes is 24; the other 320 is
a guard region that exists to hold the copies apart. Interleaving them with
`.bss` and `.noinit` gave 404 bytes of separation between the first two and only
56 between the last two, because `.noinit` is small — and 56 bytes is fourteen
words, which a wild pointer crosses without noticing. The guard brings both gaps
to roughly 400.

That is the one place in this system where RAM holding nothing is doing
something: the distance is the mechanism, not padding. And it is paid **once** —
the copies belong to three regions rather than to one variable, so a later
critical item places its copies in the same sections and costs only its own
24 bytes. The remaining 2728 B of the line returns to the reserve.

The separation protects against corruption with address locality — overruns,
runaway pointers, misaimed DMA. It does nothing against a single-event upset,
which does not walk addresses, and its value against multi-cell upsets depends
on a physical memory layout QEMU does not model. Recorded as a Phase 2 porting
obligation in ADR 0006 rather than claimed here.

**Event log, 4096 B.** The largest line overall. An event log that wraps too
quickly cannot explain an anomaly after the fact, which defeats its purpose. If
M8 shows this is more than the write path needs, the surplus returns to reserve.

**Telemetry, 2048 B.** Fixed-layout frames plus the mock sensor backend the
harness drives. Frames are compile-time defined, so this line is knowable
exactly at M6 rather than estimated — and it is: **112 B of the 2048**, measured
on `8f67feb`.

The 45-byte frame buffer is the bulk of it. The rest is the sequence counter, the
sensor readings and flags, the per-sensor run lengths the stuck detector needs,
and 4 B of `.noinit` for the mock backend. The line is left at 2048 rather than
cut back to what M6 spends: M7's command path will want to report queue depth,
M8's event log will want counters in the frame, and a line already spent is the
one place growth is cheap. The surplus is not returned to reserve yet.

**Command queue, 2048 B.** A statically sized, time-tagged queue. The size sets
the maximum number of pending commands, which is a mission parameter rather than
an implementation detail, so it should be revisited against a concrete scenario.

**Reserve, 3328 B.** Twenty percent, held back and assigned to nothing. This
exists because every estimate above except the stack is unmeasured, and because
the fault-tolerance work in M4 and M5 is exactly the kind that discovers a need
for memory late. Spending the reserve early would be the easiest way to make
this project fail at M8.

## Projection to M8

Done now rather than when the linker refuses. The two hungriest milestones are
still ahead — M7 holds 2048 bytes for the command queue, M8 holds 4096 for the
event log — and a budget that only balances in hindsight is arbitrated by
whichever milestone arrives first.

Measured on `0d1dfc2`, with every unbuilt line taken at its **full** reservation
rather than at a guess:

| | Bytes |
|---|---:|
| In use today, measured | 2044 |
| M3 panic context, at its full line | 256 |
| M7 command queue, at its full line | 2048 |
| M8 event log, at its full line | 4096 |
| **Total if every future line fills** | **8444 of 16384** |
| **Margin** | **7940** |

**It balances, with room.** No decision is needed before M7 and none is being
deferred into one.

Two figures worth having beside that, because they are where a future problem
would come from:

| | Bytes |
|---|---:|
| Reserved by the lines already built (M2, M4, M6) | 5632 |
| Actually consumed by them | 840 |
| Held and asleep | **4792** |

**The 4792 is not slack to be spent.** Most of it is the M4 line: triple-redundant
state costs three copies plus separation, and it grows every time an item becomes
critical rather than when a milestone lands. One more critical item is 24 bytes of
copies and nothing else; a dozen is still under a third of that line. The line is
sized for a system that has decided what its critical state is, and ADR 0006 says
that decision is not finished.

What this projection would look like if it did **not** balance is worth saying,
so the next person can tell: the answer would not be to shrink a line. It would
be an ADR before M7 deciding which capability the board cannot have, because
`.bss` is not negotiable at link time and the failure would arrive as a linker
error on the day M8 was otherwise finished.

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

Measured on commit `8f67feb` by `make measure`. See `docs/LOGBOOK.md` for the
run that produced these numbers.

| | Bytes |
|---|---:|
| `.data` | 16 |
| `.bss` | 592 |
| Stack reserved | 1024 |
| Stack peak observed | 112 |
| `.noinit` (fault, mode, boot records; sensor mock) | 68 |
| `.critical0/1/2` | 24 |
| `.critical_guard` | 320 |
| **Total committed** | **2044 of 16384** |

`.bss` covers the scheduler state and trace, the safe-mode mirrors, the
suspension log and the telemetry subsystem. The guard is not state: it is the
padding that keeps the three critical regions apart, and it is charged here
because it occupies RAM whatever its purpose.

Remaining unallocated after the planned lines above: 3328 B. See the projection
above for what happens when the planned lines are actually built.

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
