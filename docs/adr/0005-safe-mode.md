# ADR 0005: Safe mode, defined in terms of what exists

- **Status:** accepted
- **Date:** 2026-08-20
- **Milestone:** M3
- **Extends:** ADR 0003, which fixed the executive's properties

## Context

M3 requires a safe mode, described in the plan as "the minimum set of tasks that
keeps the system contactable".

**That phrase cannot be implemented today, and writing an ADR around it would
produce a record that has to be rewritten twice.** Being contactable requires an
uplink. There is no uplink: command ingest is M7 and telemetry framing is M6.
The only channel that exists is a transmit-only UART emitting text.

Defining safe mode against subsystems that do not exist would mean guessing at
their shape, and every guess would be a constraint on M6 and M7 written by
someone who had not built them yet.

So this record defines safe mode against the system that exists: a task table,
a cyclic executive, a serial transmitter, and a fault record that survives a
reset.

## Decision 1: safe mode is a property of the task table

**In safe mode, only tasks marked essential are dispatched. Everything else is
suspended.** Essentiality is an explicit field in the task table, not a
consequence of a task's period — a period says how often something runs, not
whether the system can do without it, and inferring one from the other would
make safe mode change silently whenever a period changed.

Against the current table:

| Task | Period | Essential | In safe mode |
|---|---|---|---|
| `housekeeping` | 1 frame | **yes** | dispatched |
| `telemetry` | 2 frames | no | suspended |
| `scrub` | 4 frames | no | suspended |
| `audit` | 8 frames | no | suspended |

`housekeeping` is essential because its continued dispatch is the only evidence
the executive is still running. When M6 gives telemetry a real frame and M7
gives commands an ingest path, both will be re-examined against this table — and
that re-examination is a change to this record, not a surprise.

The frame cadence does not change in safe mode. A degraded system that also
changes its timebase would be two failures to reason about instead of one.

## Decision 2: safe mode is reachable from three subsystems

A safe mode reachable from one place is a code path, not a mode. Three entry
points, all in code that exists today:

| Subsystem | Trigger | Path |
|---|---|---|
| Trap handler | any recorded fault | via reset, mode restored on the next boot |
| Executive | frame overrun, or a task overrunning repeatedly | direct, no reset |
| Clock | `obc_mtime_read` returning `OBC_ERR_UNSTABLE` | direct, no reset |

The trap path deliberately goes through a reset rather than entering directly.
The M1 handler touches no stack and calls nothing fallible; running mode policy
inside it would undo that property for no benefit. It records a reason and
resets, and the next boot reads the reason and comes up degraded. Policy belongs
to the boot path, not to the handler.

The other two enter directly, because nothing is wrong with the machine — only
with what it was asked to do.

## Decision 3: safe mode is observable from outside, three ways

**A system that enters safe mode without the host being able to tell cannot be
tested.** Worse, at M5 the escalation ladder has to distinguish "degraded" from
"restarted", and a mode with no external evidence makes that distinction
unavailable exactly when it is needed.

Three observables, and each answers a different question:

1. **A word in RAM** (`obc_mode`), read by the debugger. Answers *what is the
   system doing now* without perturbing it.
2. **A line on the serial stream**, emitted on entry and repeated by the
   essential task. Answers *what happened, and is it still happening*, for a
   human and for a host watching the stream.
3. **A field in the persistent fault record**, surviving a reset. Answers *did a
   previous boot go degraded*, which is the question M5 needs and which neither
   of the other two can answer across a reset.

The third is the one that would have been forgotten. It is also the reason the
trap path can go through a reset at all: without persistence, a fault that
triggers safe mode and resets would come back up nominal, having lost the fact.

## Consequences

**Safe mode is one-way within a boot.** Nothing exits it except a reset. Exit
policy needs a decision about *what evidence justifies resuming*, and that
evidence does not exist before M7 gives the ground a way to say so. Recorded
here so that "it never exits" reads as a decision rather than an omission.

**The essential flag widens the task table**, which widens the compile-time
checks: at least one task must be essential, or safe mode is a stopped system
wearing a different name. That is a static assertion, not a runtime check.

**M5 inherits a distinction it can act on.** Degraded-and-running, degraded-
after-reset, and reset-loop are three different states with three different
pieces of evidence, and the escalation ladder can tell them apart.

**M6 and M7 will revisit the essential set**, and this record expects that. What
they must not do is redefine what safe mode *means*: the mechanism is "dispatch
only the essential subset, observably"; which tasks are in that subset is data.

## Alternatives considered

**Defining safe mode as "keeps the system contactable".** Rejected as
unimplementable today, and worse than unimplementable: it would have produced a
definition in terms of a command path whose shape is decided at M7, constraining
that milestone from a position of ignorance.

**Inferring essentiality from the period.** Rejected: it couples two independent
properties, so changing how often a task runs would silently change what the
system does when degraded.

**Entering safe mode directly from the trap handler.** Rejected: it would put
policy in a handler that must touch no stack and call nothing fallible, and the
persistent record already provides the mechanism to defer it to the boot path.

**Halting instead of degrading.** Rejected: a halted system is indistinguishable
from a hung one over a transmit-only serial line, which is the whole channel
available. A system that keeps dispatching one task and says why is observably
different from one that stopped.
