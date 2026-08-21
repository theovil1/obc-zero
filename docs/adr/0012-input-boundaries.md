# ADR 0012: Three boundaries the command path has to draw

- **Status:** accepted
- **Date:** 2026-08-21
- **Milestone:** decided before M7 is written
- **Extends:** ADR 0011, which settled that an input is not a fault

## Context

ADR 0011 drew the first line: on a fault surviving is recovering, on an input
surviving is refusing and changing nothing else, and therefore the command path
never enters the escalation ladder.

Three consequences of that line need settling before code, because each is cheap
now and each is the kind of thing discovered by a counter that has gone round, an
assertion in the wrong place, or a campaign that measured the wrong binary.

## Decision 1: counters written by the outside saturate, never wrap

A rejection counter is **memory the outside writes**. Somebody transmitting
continuously drives it, and two things follow that do not follow for any counter
this project has built so far.

**It wraps if it is allowed to.** A 16-bit counter that has gone round reads as a
small number, so a sustained flood reports as a handful of rejections — a large
problem shown as a small one, which is worse than showing nothing. **Saturating
means "at least this many"**, and at the ceiling the value is still true.

The precedent exists: `sat16` and `sat8` in `flight/tlm/frame.c` already do this
for the counters telemetry publishes, for the same reason and without the
adversary. Here it stops being prudence and becomes the point.

**And the frame stays the size it was.** ADR 0011 decision 3 already bounds
console output by the number of rejection reasons rather than by the number of
frames. The same bound applies to telemetry: one counter per reason, a fixed set
fixed at compile time. **Nothing the outside sends may change how many fields a
frame has**, because a frame whose shape is attacker-influenced is a frame the
ground decodes against a layout the vehicle chose under someone else's direction.

What this does *not* claim: saturation is not a defence. A flood still costs the
uplink, the dispatch and the operator's attention. It is a guarantee about the
*report* — that the number in the frame is never a lie — and nothing more.

## Decision 2: an argument's range is a build property; rejecting an argument is a flight decision

These are two different things that share a word, and the confusion is one
keystroke deep in either direction.

| | Belongs to | Checked | Because |
|---|---|---|---|
| `min <= max` for every argument | the build | **on the ground, against the binary** | a vehicle cannot repair a table it shipped with |
| every opcode has a handler | the build | **on the ground, against the binary** | same |
| no duplicate opcodes | the build | **on the ground, against the binary** | same |
| the declared argument count fits the frame | the build | **compile-time assertion** | expressible in C, so it costs nothing |
| *this* argument is inside *its* declared range | the flight | **a refusal, every time, in flight code** | it depends on what arrived |

The M6 rule says what is decided at build time is verified on the ground. **The
subtle case is that argument validation looks like both.** The range is a
compile-time constant sitting in a table; the decision about a particular
argument is not, and cannot be, because the argument arrives from outside.

Two mistakes, and they fail in opposite directions:

- **A static assertion where a refusal belongs.** The build checks that the table
  declares a range, everybody reads that as "arguments are validated", and the
  vehicle executes whatever number arrives. The check is real, it passes, and it
  guards nothing that happens in flight.
- **A refusal where a static assertion belongs.** The vehicle checks at boot that
  its own table is well formed and degrades when it is not — which is the defect
  M6 shipped and removed within the hour: a system that cannot grow a handler
  turning a desk mistake into a degraded mission.

**The test that separates them** is the same both-directions rule this repository
keeps returning to: an out-of-range argument must be rejected **and** an in-range
one must be executed. A validator that rejects everything passes the first half,
and it is the half that gets written.

## Decision 3: the fuzz campaign runs on the image that flies

No stub, no substituted source, no test-only path.

This is not a general preference — the harness substitutes sources all the time,
and `harness/broken/` exists for exactly that. It is specific to a **robustness**
campaign, because such a campaign's claim is about the binary's behaviour under
arbitrary input, and a binary that is not the one that flies supports no such
claim.

**The cost of ignoring this is measured, not supposed.** The M6 downlink stall
substitute added five instructions to a five-instruction poll loop. The stalled
dispatch went from 2746 to 3949 against a 3000 budget, overran, climbed the
ladder and reset the machine. The instrument moved the system across the exact
threshold under test. A hundred thousand frames through an instrumented parser
would be a hundred thousand results about a parser nobody ships.

**So it is a refusing control, and it runs before the campaign, not after.** The
corollary in force since M4: a refusal that arrives after the expensive work is a
degraded refusal. Checking the link map costs milliseconds; a fuzz campaign does
not.

Implemented as `make flight-image-check`, which reads `build/obc.map` and refuses
if any object outside `flight/` and `emu/` was linked. It guards every campaign,
not only M7's — the voter campaign has always been meant to run on the flight
image and had nothing saying so.

## Consequences

**The rejection reasons are a closed set fixed at compile time.** One counter
each, one telemetry field each, one console announcement on first occurrence
each. Adding a reason is a layout change the ground picks up from the binary,
which is what M6 built and why.

**M7's report says which binary it ran on**, by hash, beside the provenance and
timing-basis headers. A campaign that cannot name its binary cannot support a
claim about that binary.

**Nothing here defends against a flood.** Decision 1 is explicit that saturation
is a property of the report. Rate limiting, if the mission ever needs it, is a
separate decision with its own record, and it is deliberately absent rather than
forgotten.

## Alternatives considered

**Let the counters wrap and publish a wrap flag.** Rejected: it is an extra field
and an extra thing for the ground to handle correctly, to represent a state that
saturation already represents truthfully. The M6 sequence counter breaks
monotonicity deliberately because the break is *impossible to ignore*; a wrap
flag is easy to ignore.

**Validate arguments at boot against the table and cache the verdict.** Rejected
under decision 2: the verdict depends on the argument, so there is nothing to
cache. It reads as an optimisation and is a category error.

**Run the fuzz campaign on an instrumented build for the coverage.** Rejected
under decision 3, and worth naming because the instrumented build would genuinely
see more. What it would see is more about a binary that does not fly.
