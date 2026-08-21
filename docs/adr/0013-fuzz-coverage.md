# ADR 0013: A green fuzz campaign has to prove it did something

- **Status:** accepted
- **Date:** 2026-08-21
- **Milestone:** decided before M7 is written
- **Extends:** ADR 0011, ADR 0012

## Context

M7 is the first milestone whose success criterion is that **nothing happens**,
a hundred thousand times in a row.

Every campaign before it asserted a change: a copy was repaired, a mode was
entered, a machine was reset. This one asserts an absence, and that inverts the
relationship between the result and the evidence.

**A fuzzer that never reaches the parser produces exactly the same result as a
perfect parser.** Both give a hundred thousand frames and no effects. So does a
uplink that dropped every frame, a harness that sent to the wrong port, a build
where the command task was never dispatched, and a parser that silently discards
everything it does not understand.

Without a coverage obligation, M7 would produce the greenest and least
informative result in the repository, and the greenness would be indistinguishable
from the failure it is supposed to exclude.

This is the bidirectional rule in the shape M7 gives it. The rule says the
forgotten direction is the one requiring an effect to be observed. Here **the
whole assertion is an absence**, so the effect that has to be observed is not the
system's — it is the campaign's own.

## Decision 1: the evidence comes from the flight image, and that is not a
constraint

ADR 0012 forbids running a campaign through an instrumented binary, which appears
to forbid measuring coverage at all: coverage instrumentation is instrumentation.

It does not, because **the counters are already flight code.** A rejection counter
is published in telemetry — ADR 0011 decision 3 — so it exists in the image that
flies for reasons that have nothing to do with testing. Reading it is reading a
frame, not instrumenting a binary.

That is a happy accident of an earlier decision rather than foresight, and it is
worth saying which: had rejection counting been built as a test-only affordance,
M7 would have had to choose between coverage and ADR 0012.

## Decision 2: four obligations, and the third is the one that matters

A campaign is only allowed to report success if all four hold.

**1. The parser was reached.** The vehicle's `frames_examined` counter equals the
count of frames the harness sent. Two independent counts, one at each end, and
they must agree. This is what catches an uplink that dropped everything, a
harness pointed at the wrong port, and a command task that was never dispatched.

**2. Nothing fell between.**

```
frames_examined == frames_accepted + Σ frames_rejected[reason]
```

A frame that was examined and is in neither column was *silently discarded*, and
silent discard is exactly what a passing campaign looks like from outside. This
identity is the crisp form of the whole record and the one assertion that cannot
be satisfied by doing nothing.

**3. Every rejection reason was reached, more than once.** A closed set of reasons
is fixed at compile time; each must be reached at least `OBC_FUZZ_REASON_FLOOR`
times.

**Not "at least once."** One hit per reason and 99 990 on a single one is
technically covered and practically not: it says the fuzzer found the cheapest
rejection and stayed there. The floor is **declared arbitrary**, on the same
footing as `OBC_SHORT_BOOT_LIMIT` and `OBC_SENSOR_STUCK_LIMIT`, and the *whole
distribution* is published so that a skewed campaign is visible even when it
passes.

**4. The accept path was exercised.** `frames_accepted > 0`, and the commands
accepted actually executed. A fuzzer that never assembles a well-formed frame
leaves the accept path untested, and then "nothing happened" is true because
nothing was ever asked to happen.

Obligation 4 is the both-directions rule applied to the campaign itself: rejecting
correctly proves nothing unless accepting correctly is proven beside it.

## Decision 3: the report publishes the distribution, not the totals

A total is what a skewed campaign hides behind. The M7 report carries one row per
rejection reason with its count, the accepted count, and the examined total, and
the identity from obligation 2 is shown as arithmetic the reader can check rather
than as a claim that it held.

The seed goes in the header as every campaign's does, and it is what makes the
distribution replayable rather than anecdotal.

## Consequences

**`frames_examined` is a flight counter and costs RAM**, charged to the M7 line
in `docs/BUDGET.md`. It is not test scaffolding and is not compiled out, for the
same reason the execution trace is not: the image that flies is the image that
was measured.

**A campaign can now fail for a reason that is not a defect in the vehicle.** A
fuzzer that fails obligation 3 has found nothing wrong with the command path; it
has found something wrong with itself. Those must be reported differently, or the
first skewed campaign will be read as a system failure and the second as noise.

**What this does not prove.** Reaching a rejection reason is not covering the
branches inside its check. A range test with an off-by-one at its upper bound is
reached, counted, and wrong. Branch coverage on bare metal needs instrumentation,
which ADR 0012 forbids for campaigns, so the gap is real and named rather than
papered over. What closes it is targeted cases at the boundaries — written by
hand, not fuzzed — and they belong beside the campaign rather than inside it.

## Alternatives considered

**Count frames sent and call that coverage.** Rejected: it is the number the
harness already knows and says nothing about the vehicle. It is also the number
that would be reported if the uplink were disconnected.

**Require a minimum number of *distinct* frames.** Rejected as a proxy: distinct
inputs can all take one path, and the property wanted is about paths taken rather
than about inputs supplied. The reason distribution measures the thing directly.

**Instrument the parser for branch coverage and run the campaign on that build.**
Rejected under ADR 0012. It would measure a binary nobody ships, and the M6 stall
substitute already showed what an instrument costs when it lands on the threshold
under test.
