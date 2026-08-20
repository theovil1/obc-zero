# ADR 0002: Two time domains, and the use of `-icount`

- **Status:** accepted
- **Date:** 2026-08-20
- **Milestone:** decided before M1, binding on M2 and M9
- **Supersedes:** nothing. Extends ADR 0001.

## Context

ADR 0001 established that the machine timer runs at **32768 Hz**. One tick is
therefore **30.5 µs**, and that number turns out to decide more than timekeeping.

M2 requires a per-task execution budget: a task exceeding its budget is a
recorded fault. But a task that runs in a few microseconds measures zero or one
tick. **`mtime` cannot measure a task budget at all**, and a budget mechanism
built on it would report zeros and pass every test by accident.

So a second time source is needed. The RISC-V hardware performance counters
`mcycle` and `minstret` are the candidates. Both raise a question that has to be
answered before M2 writes anything, because the answer also decides M9.

## Measurements

All on QEMU 10.2.1, `sifive_e`, four runs of an identical workload.

**Without `-icount`, the counters are noise.** The same 1000-iteration loop:

| Run | `mcycle` delta | `minstret` delta |
|---|---:|---:|
| 1 | 97664 | 113434 |
| 2 | 92658 | 91884 |
| 3 | 102682 | 100708 |
| 4 | 93378 | 92170 |

A 12 % spread on `mcycle`, and `minstret` is no better: QEMU is reporting host
behaviour, not guest instructions.

**With `-icount shift=0`, both are exact and reproducible.** The same workload
gives `mcycle = minstret = 6006` on every run, without variation.

Note what that equality means: under `-icount`, QEMU advances one cycle per
instruction. **`mcycle` is not a cycle count.** It is an instruction count
wearing a cycle count's name, and it carries no information about pipeline
behaviour, cache behaviour, or wall-clock duration.

**Execution position becomes reproducible.** Attached through the gdbstub,
stepping a fixed number of instructions from a breakpoint:

| | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| Without `-icount` | `minstret=644399082` | `minstret=-744588599` | `minstret=2138342361` |
| With `-icount shift=0` | `minstret=1096` | `minstret=1096` | `minstret=1096` |

**The counters are writable, contrary to expectation.** Writing `0x40000000` to
`mcycle` and reading it back one instruction later returns `0x40000001` under
`-icount`. The write takes effect exactly. Without `-icount` it also takes
effect, but the read-back has drifted by ten thousand host cycles, which is
another way of seeing that the counter is not measuring the guest.

This was expected to be silently ignored. It is not, on this version. The carry
test technique used for `mtime` therefore applies to these counters too, and the
acceptance criteria may rely on presetting them.

## Decision

**Run everything under `-icount shift=0`.** Emulation, tests and fault campaigns.

**Two time domains, with separate and non-overlapping authority:**

| Domain | Source | Authoritative for | Never used for |
|---|---|---|---|
| Real time | `mtime`, 32768 Hz | Scheduling, deadlines, timestamps, anything published in telemetry or a report | Task execution budgets |
| Execution effort | `minstret` under `-icount` | Per-task budget enforcement, fault injection coordinates | Any published duration, any timing claim |

The second domain measures **work done**, not time elapsed. A budget expressed
in it says "this task executed more instructions than it was allowed", which is
a meaningful and reproducible statement. It does not say "this task took 3 ms",
which would be a fabrication.

**No figure from the execution domain is ever published as a duration.** Not in
telemetry, not in a campaign report, not in the logbook. If a report needs a
duration it comes from `mtime`, with the resolution stated.

`minstret` is preferred to `mcycle` for budgets. Under `-icount` they are the
same number, and `minstret` is the one whose name does not invite a false
interpretation the day the code runs on hardware.

## Consequences

**M2 budgets are expressed in retired instructions, not milliseconds.** The task
table carries an instruction budget. This is a change to how M2 was described
and is the reason this record exists before M2 rather than during it.

**M9 gets its determinism, and the open question from ADR 0001 is closed.** That
record noted that the gdbstub attaches in wall-clock time, so a seeded injector
would land on a different instruction every run, and left the resolution to be
decided at the start of M9. `-icount` decides it: an injection point is a
`minstret` value, and the same seed reaches the same instruction every time.
No TCG plugin is required.

**Deciding this now was the point.** Adopting `-icount` after M2 had measured
budgets and M9 had built injectors would invalidate the measurements of both.
The cost of the decision is one build flag; the cost of deferring it is two
milestones of results.

**On hardware, both domains change meaning.** `mcycle` becomes a true cycle
count and stops equalling `minstret`; `mtime` keeps its 32768 Hz definition.
Budgets calibrated in emulation are therefore a starting point for the port, not
a specification. The Phase 2 port must recalibrate them and say by how much they
moved.

## Alternatives considered

**`mtime` alone, with coarser budgets.** Give each task a budget of several
ticks so 30.5 µs resolution suffices. Rejected: it forces tasks to be padded to
measurable length, which distorts the design to fit the instrument, and it still
cannot distinguish a task that ran 1 µs from one that ran 30 µs.

**Wall-clock measurement from the host.** The harness times each task from
outside. Rejected: it measures the host, and ADR 0001 already records that serial
and wall-clock timing through QEMU are not trustworthy for assertions.

**A TCG plugin for instruction-accurate injection.** Considered for M9 before
`-icount` was measured. Rejected as unnecessary: `-icount` gives the same
determinism through a supported flag, with no plugin to build or maintain.

**Running without `-icount` and accepting nondeterminism in budgets only.**
Rejected outright. A budget that fires 12 % of the time depending on host load
is not a fault detector, it is a source of unexplained campaign failures.
