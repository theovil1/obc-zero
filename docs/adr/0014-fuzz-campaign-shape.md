# ADR 0014: Many short runs, and what that does not exercise

- **Status:** accepted
- **Date:** 2026-08-21
- **Milestone:** decided before the M7 campaign is run
- **Extends:** ADR 0012, ADR 0013

## Context

M7's criterion asks for 100 000 malformed frames. It does not say over how many
runs, and the two readings do not prove the same thing.

The uplink carries six to twelve frames per run — throughput is set by the
receive FIFO and the poll period, not by the baud rate, and the figure depends on
host load. So 100 000 frames is roughly ten thousand runs of the current image,
or many fewer runs of an image with a longer window.

**Both satisfy the letter of the criterion, and a report saying "100 000 frames"
without saying which leaves nobody able to tell what was exercised.**

## Decision: many short runs

Ten thousand runs of the flight image, seeded and replayable, on the same
infrastructure the M4 voter campaign uses.

**The reason is not the boot coverage, welcome as it is.** It is ADR 0012
decision 3: a campaign runs on the image that flies. Lengthening the window means
changing `OBC_SCHED_WINDOW_FRAMES`, which changes the image — and a robustness
result about a binary with a different scheduler window is a result about a
different binary.

That the window is bounded at all is itself an artefact this project has been
carrying since M2, and the tension is worth naming rather than resolving here:
**in flight the executive does not return.** The 16-frame window exists so the
milestone's assertions can be made over something finite. M3 was to supply the
endless outer loop and did not. So the honest position is that the campaign
measures what ships, and what ships has a bounded window.

## What the shape exercises, and what it does not

| | Many short runs | Fewer long runs |
|---|---|---|
| Frames through the parser | the same | the same |
| The boot path | **10 000 times** | a handful |
| The first-frame-after-boot path | 10 000 times | a handful |
| Counters near their ceilings | never — each run starts at zero | **yes** |
| A replay across a boot | never — the counter resets | **yes** |
| Long-running state drift | **not at all** | yes |

The right column is not hypothetical. Three of those rows are real gaps in what
this campaign will prove, and they are stated here so the report can say so
rather than imply coverage it does not have:

- **Saturation is untested by it.** ADR 0012 decision 1 made the rejection
  counters saturate precisely for a sustained flood, and no run in this campaign
  gets within four orders of magnitude of a 32-bit ceiling. The behaviour at the
  ceiling remains an argument, not a measurement.
- **Cross-boot replay is untested by it.** The uplink counter resets on every
  boot, so a frame replayed after a reset is accepted. That is a real property of
  the current design — the counter does not survive a reset — and this campaign
  cannot see it because every run is a fresh boot.
- **Nothing here runs long enough to drift.** That is M10's soak, and it is a
  different question.

## Consequences

**The report states the shape in its header**, beside the seed, the provenance,
the timing basis and the binary hash. "100 000 frames over 10 000 runs of 12" is
a different claim from "100 000 frames over 8 runs", and a reader aggregating
campaigns needs to see which without reading prose.

**The per-run identity is what aggregates.** ADR 0013's
`examined == accepted + sum(rejected)` holds per run, and the campaign's totals
are sums across runs. A run that fails it fails the campaign; the totals cannot
paper over a single run where a frame went missing, because the sum of correct
identities is a correct identity and one bad run breaks it.

**Cross-boot replay goes to the backlog, named.** It is not a fuzzing gap, it is
a design question — should the uplink counter survive a reset, and in which
record? M8 has a persistent store and is where it can be answered.

**Roughly four hours of host time** at a second and a half per run. Overnight,
like the M10 soak, and the same caution applies: a campaign that cannot survive
its machine being rebooted is a campaign that produces nothing. Checkpointing
after each run rather than at the end.

## Alternatives considered

**Fewer, longer runs, by raising `OBC_SCHED_WINDOW_FRAMES` for the campaign.**
Rejected under ADR 0012 decision 3. It would also test counters near their
ceilings, which is the one thing it does better and which is not worth measuring
a different binary for.

**Both shapes, reported separately.** Not rejected, deferred: the long-run shape
becomes free once the executive has an endless loop, and building it now would
mean building the changed image this record just declined to measure.

**Fewer frames.** Rejected: the criterion says 100 000 and the arithmetic works.
Cutting it because the runs are slow would be choosing the number from the
convenience rather than from the property, which is the failure this repository
keeps finding in its own thresholds.
