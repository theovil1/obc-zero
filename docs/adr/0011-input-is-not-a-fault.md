# ADR 0011: A malformed command is not a fault

- **Status:** accepted
- **Date:** 2026-08-21
- **Milestone:** decided before M7 is written
- **Extends:** ADR 0005 (safe mode), ADR 0007 (escalation), ADR 0009 (emission)

## Context

Everything this project has built so far responds to **involuntary failure**. A
bit flips. A clock will not settle. A task runs long. A boot dies early. The
whole apparatus — the voter, the ladder, the watchdog, safe mode — exists to
recover from something that went wrong on its own.

M7 is the first milestone where the system **accepts something from outside**.

A malformed command is not a bit flip. Nothing failed. The uplink delivered
exactly what it was given, the parser received exactly what was sent, and every
component behaved correctly. The frame is simply not a command.

That difference has to be settled before any of it is written, because it changes
what the word *survive* means, and every mechanism M7 builds will be built to one
meaning or the other.

## Decision 1: two properties, two words, and they are not interchangeable

| | **Fault tolerance** | **Robustness** |
|---|---|---|
| The event | something failed | something arrived |
| Cause | involuntary | possibly deliberate |
| Surviving means | **recovering** | **refusing, and changing nothing else** |
| Success looks like | the system continues in a degraded but defined state | the system is bit-for-bit as it was, plus one rejection record |
| Wrong response | ignoring it | escalating |

**On a fault, surviving is recovering.** The voter repairs a copy. The ladder
suspends a task. The system arrives somewhere new and defined, and that movement
is the point.

**On hostile input, surviving is refusing without changing anything else.** A
rejected command must leave the vehicle in the state it was in — same mode, same
queue, same counters except the one counting rejections. Any other movement is
the input having an effect, which is precisely what it must not have.

The failure mode this distinction prevents is concrete and would otherwise be
easy to build: **a malformed frame that drives the escalation ladder.** Every
piece would be individually defensible — the parser detects a fault, the fault
escalates, the ladder does its job — and the result is a vehicle that can be
reset by anyone who can transmit garbage at it. A denial of service assembled
entirely from correct components, which is the shape of every defect this
repository has found.

**So: the command path never enters the escalation ladder.** A rejection is not a
fault, is not counted as one, and cannot reach rung 1. If the command *subsystem*
itself misbehaves — overruns its budget, corrupts its own queue — that is a fault
and the ladder is exactly right. The line is between the input and the machinery
that handles it, and it will need saying again every time somebody adds a check.

## Decision 2: the fuzzing criterion tests robustness, and says so

`PLAN.md` asks for 100 000 malformed frames. **That is not a fault-injection
campaign** and calling it one would put it in the wrong column of the report.

| | Fault campaign (M4 voter) | Fuzz campaign (M7) |
|---|---|---|
| Input | a corruption chosen by seed | a frame chosen by seed |
| Passing means | the system recovered | the system did not move |
| A failure is | an unrecovered corruption | a frame that had *any* effect |

Both are seeded and both are replayable — that part of the discipline transfers
unchanged. What does not transfer is the assertion, and the M7 report must not
inherit the M4 report's vocabulary.

**What "did not move" is asserted against**, concretely, and it is the hard part:

- the mode, read through the voter, is what it was;
- the command queue holds exactly what it held;
- the task table's dispatch pattern over the following window is unchanged;
- no suspension is logged, no rung is taken, no boot occurs;
- **the rejection counter moved by exactly one**, and its reason field says which
  check refused.

The last one is the direction that gets forgotten, per the rule this repository
keeps relearning. "Nothing happened" is cheap to assert and is also what a parser
that silently discarded the frame would produce. **A rejection has to be
observable, or the system is not refusing, it is ignoring.**

## Decision 3: rejection is not silent, and not chatty either

A rejected command produces a counted, reasoned record. It does **not** produce a
console line per frame: at 100 000 frames that is a denial of service against the
log, and against the campaign's own ability to be read.

So: a counter per rejection reason, published in telemetry, and the *first*
rejection of each reason announced. That bounds the output by the number of
reasons rather than by the number of frames, which is the property that survives
someone transmitting garbage for an hour.

## Decision 4: an accepted command is still not trusted

Validation and authorisation are different questions and M7 answers only the
first. A frame that parses, checksums and fits the schema is **well formed**; it
is not thereby *authorised*, and this record does not claim it is.

There is no authentication in M7 and none is being smuggled in under the word
"validated". If the mission ever needs one, it is a separate decision with its own
record, and the gap is named here so that nobody reads a green fuzz campaign as
evidence of something it never tested.

## Consequences

**`docs/reports/` gains a second report shape.** The M7 fuzz report states what
did not move, not what was recovered. Its provenance and timing-basis headers
carry over unchanged.

**The M6 emission rule does not apply here, and the contrast is the point.** ADR
0009 decided that telemetry sheds its data rather than the system, because
telemetry is observability. **Commands are control, and the answer inverts**: a
command that cannot be validated is not shed quietly, because losing one silently
is losing authority over the vehicle. It is rejected loudly — counted, reasoned,
published — and never dropped on the floor.

That inversion was predicted in ADR 0009 and is confirmed here rather than
rediscovered, which is the only reason writing it down early was worth anything.

**`docs/EMULATION-GAP.md` may gain an entry.** The uplink does not exist on this
machine either; whatever stands in for it will be a harness fiction, and if that
fiction is kinder than a real radio the gap belongs in that file at the milestone
that finds it, not later.

## Alternatives considered

**Treat a malformed command as a fault and let the ladder handle it.** Rejected
under decision 1. It is the option that requires no new machinery, which is
exactly why it is tempting, and it hands the reset button to anyone with a
transmitter.

**Reject silently and count nothing.** Rejected: indistinguishable from a parser
that lost the frame, and it makes the fuzz campaign unfalsifiable — a system that
discarded every input would pass.

**Fold the fuzz campaign into the existing fault-injection report.** Rejected:
the two have opposite pass conditions. One report saying "recovered 100 000
times" and "moved zero times" in the same table is a report whose reader has to
know which rows mean which, and in six months nobody will.
