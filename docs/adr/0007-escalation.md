# ADR 0007: The escalation ladder, and what it can actually measure

- **Status:** accepted
- **Date:** 2026-08-21
- **Milestone:** decided before M5 is written
- **Extends:** ADR 0005, which defined safe mode

## Context

M5 adds recovery from hangs: a watchdog, an escalation ladder, and protection
against reset loops. Three questions have to be answered before any of it is
written, because each one is cheap now and expensive once code assumes an answer.

## Decision 1: the ladder has two rungs, and the third is named and empty

`PLAN.md` describes three: restart the task, reset the subsystem, reset
everything. **The middle rung has nothing to act on.** A subsystem reset means
returning a subsystem's own state to a known value, and no subsystem owns state
yet — telemetry arrives at M6, the event store at M8.

Inventing a middle rung today would produce something that reads as escalation
and does nothing, which is worse than an absent rung: an absent rung is visible
in the ladder, a hollow one is visible only in a campaign, late.

So the ladder is:

| Rung | Action | Available |
|---|---|---|
| 1 | Suspend the offending task for a frame | **yes** |
| 2 | Reset the offending subsystem | **named, empty until M6/M8** |
| 3 | Reset the machine through the AON watchdog | **yes** |

The empty rung is declared in the code as an enumerated value with no
implementation and a compile-time assertion that nothing selects it. It exists
so that M6 fills a hole rather than inserting a step, and so that the numbering
in reports stays stable.

### Rung 1 must be observable or it does not exist

A task with no persistent state is "restarted" by not calling it for one frame,
which is indistinguishable from the scheduler simply not calling it. **An
unobservable recovery action is not an action.**

So rung 1 is defined by its observables rather than by its effect:

- the suspended task's index and the frame it was suspended in are recorded;
- the execution trace shows the gap, and the host's conformance checker is told
  to expect it — a suspension the checker does not know about is a dropped
  dispatch, which M2 correctly treats as a defect;
- a counter of suspensions per task, reported.

If those three are absent, rung 1 is a comment. The M5 test asserts the gap
appears in the trace at the expected frame, not merely that a counter moved.

## Decision 2: the window is counted in boots, not in time

Reset-loop protection needs "N resets within a window". The obvious window is
elapsed time, and **it cannot be measured here.**

`mtime` does not survive a reset: it restarts from zero. There is no clock that
spans a reboot, no RTC in the machine model, and the AON backup registers that
would hold one on real hardware are not implemented — recorded in ADR 0001. So
the system knows how long it has been up, and nothing about how long ago the
previous boot ended.

Writing "five resets in ten minutes" into this record would produce a criterion
that cannot be evaluated, discovered while implementing it.

**The window is therefore a count of consecutive short boots.** A boot is short
if it ends before `OBC_SHORT_BOOT_TICKS` of uptime. A counter in persistent
state increments on each short boot and **resets to zero on the first boot that
is not short**. Reaching the threshold means the system has failed to stay up N
times in a row.

This is a different property from "N resets in T minutes" and it is the honest
one:

- it does not detect a system that resets once an hour, forever. That is a
  distinct failure and needs a clock that survives reset, which is a Phase 2
  concern with real hardware.
- it does detect the failure that matters here — a system that cannot complete a
  boot, which is what a reset loop is.
- it is measurable with the clock that exists, and stated in the units it is
  measured in.

**The threshold and the short-boot duration are declared arbitrary** and are
recorded as figures to calibrate against a real mission profile, not derived
from anything.

## Decision 3: safe mode is the top of the ladder, not a parallel mechanism

M3 built a safe mode reachable from three subsystems. M5 builds a ladder ending
in a reset. **If those coexist without a hierarchy, the system has two survival
mechanisms that can contradict each other** — and the contradiction only appears
in a campaign where both trigger, which is late and hard to read.

So: **safe mode is what the ladder reaches when resetting has stopped helping.**

```
rung 1  suspend a task
rung 2  (empty until M6/M8)
rung 3  reset the machine
        ↓  N consecutive short boots
      safe mode, and it does not exit
```

Concretely:

- The reset-loop counter reaching its threshold enters safe mode **instead of**
  resetting again. A reset that has not worked N times in a row is not going to
  work on attempt N+1, and continuing costs the only thing left — the ability to
  be observed.
- The three M3 entries are unchanged and remain direct. They are not rungs; they
  are conditions under which the system already knows it should be degraded, and
  a system that knows that should not climb a ladder to find out.
- Safe mode still does not exit, per ADR 0005. Nothing below it can be reached
  from it.

The single ordering rule, so it can be applied without re-deriving it: **the
ladder escalates towards safe mode, and never away from it.**

## Consequences

**The reset counter needs its own record**, which the backlog already recorded:
it must survive `obc_fault_consume()`, so it cannot share the fault record's
magic and checksum. Its own record, its own magic, its own checksum.

**The conformance checker learns about suspensions.** Rung 1 creates a legitimate
gap in the execution trace, and M2's checker treats any gap as a dropped
dispatch. The suspension record is what tells the two apart, and if that record
is wrong the checker will report a scheduler defect that does not exist — a
false red, which this project has already learned is the expensive kind.

**A campaign that reaches rung 3 is a campaign that reboots**, and the harness
must expect several banners rather than exactly two. The trap tests assert
exactly two today and will need to state which count they expect and why.

## Alternatives considered

**A middle rung that reinitialises the scheduler's own counters.** Rejected: it
would be a rung that acts on the recovery mechanism rather than on a subsystem,
which inverts what escalation means and would make the ladder self-referential.

**Elapsed-time window using a tick value persisted across reset.** Rejected: the
persisted value would be the *uptime at the moment of reset*, which says nothing
about the gap between boots. Summing uptimes measures how long the system ran,
not how long ago it ran, and the difference is exactly what a loop window needs.

**Safe mode as a rung below reset.** Rejected: degrading before trying a reset
would suspend work in response to a fault a reset might clear, and would make
the deepest state reachable the one entered first.
