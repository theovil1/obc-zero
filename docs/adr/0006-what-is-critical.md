# ADR 0006: What counts as critical state

- **Status:** accepted
- **Date:** 2026-08-21
- **Milestone:** decided before M4 is written
- **Extends:** ADR 0005

## Context

M4 stores critical state in triplicate and reads it through a voter. Three
copies of anything cost three times its size, and `docs/BUDGET.md` allots the
line 3072 bytes out of 16384 — meaning **1024 bytes of actual state**, and this
is the first milestone where the budget can genuinely refuse.

The failure mode to avoid is not running out of memory. It is deciding what
"critical" means *while* running out of memory, at which point the word quietly
comes to mean "whatever fit". A definition written after the link fails is a
definition shaped by the link.

So the criterion goes here, before any of it is written, and the list of state
it admits is derived from it rather than assembled and then justified.

## The criterion

State is **critical** if, and only if, all three hold:

1. **A single-bit corruption of it can change what the system does**, not merely
   what it reports. State that only affects a message is telemetry, however
   embarrassing a wrong message would be.
2. **The wrong behaviour would not be self-evident.** If a corruption puts the
   system into a state that the next check catches anyway, the check is already
   the protection and triplication adds cost without adding detection.
3. **It cannot be recomputed from something more trustworthy.** Derived values
   are protected by protecting their source. Storing a derived value three times
   is protecting a shadow.

A fourth clause is a veto rather than a requirement:

4. **It must be small enough that three copies are affordable at their true
   cost**, counted against the line and not against the reserve. State that only
   fits by spending the reserve is state whose criticality has not been argued
   hard enough.

## What the criterion admits today

Applied to the state that exists at the end of M3.

| State | Clause 1 | Clause 2 | Clause 3 | Critical |
|---|---|---|---|---|
| `obc_mode` | yes — decides which tasks dispatch | yes — nothing re-checks it | yes | **yes** |
| `obc_safe_reason` | no — explains, does not decide | — | — | no |
| Fault record | yes — decides the boot's mode | yes | yes | **already protected** |
| Mode record | yes — same | yes | yes | **already protected** |
| Task table | yes — decides everything | yes | yes | **in flash, immutable** |
| `obc_task_state` counters | no — reported, never acted on | — | — | no |
| Execution trace | no — observability | — | — | no |
| Frame counters, slack | no — reported | — | — | no |
| `obc_safe_entry_frame` | no — a host-side hint | — | — | no |

**Exactly one live variable qualifies: `obc_mode`.** Four bytes.

That is a result, not a disappointment, and it is worth stating why. A corrupted
`obc_mode` silently returns a degraded system to nominal — it resumes
dispatching tasks that were suspended because something was wrong, and nothing
in the system re-derives the mode to notice. It is the clearest instance of
clause 2 in the codebase.

The two persistent records already carry a magic and a checksum, which is a
different mechanism for a different threat: they must survive a reset and be
rejected when they are noise, rather than be voted on while the system runs. The
criterion does not ask for them to be triplicated as well, and doing so would be
protecting the same state twice with the second mechanism adding nothing.

The task table is immutable and lives in flash. A corrupted RAM word cannot
reach it, which is why it was put there.

## Consequences

**M4 uses a small fraction of its 3072-byte line, and the surplus returns to
the reserve rather than being spent because it was allocated.** An allocation is
a ceiling, per the rules in `docs/BUDGET.md`, and a milestone that comes in
under its ceiling has done the budget a favour, not left value on the table.

**The mechanism is built for more than it currently protects.** Voting over one
variable would be a strange thing to write on its own; the value is that M5, M7
and M8 each arrive with state that has to be argued against this criterion
rather than assumed into the structure. The list above is expected to grow, and
each addition is a change to this record.

**The criterion is a filter, not a list.** Later milestones apply the four
clauses and record the verdict, including the negative ones. A state that was
considered and rejected is more useful to a reader than a list that only shows
the survivors.

**Triplication does not replace the checksum on persisted records**, and neither
replaces the other. Voting answers "which of three disagreeing copies is right,
now"; a checksum answers "is this word worth believing at all, after a reset".
Different questions, and conflating them would leave one of the two unanswered.

## Alternatives considered

**Triplicate everything in `.bss`.** Rejected on arithmetic before principle:
`.bss` is 396 bytes today and every milestone from M5 to M8 adds to it. It also
inverts the burden of proof, making criticality the default and forcing the
argument onto whatever is left out.

**Decide criticality per milestone, as each arrives.** Rejected: that is the
same as deciding it while the budget is under pressure, which is the failure
this record exists to prevent. The criterion is fixed now precisely so that M8
cannot argue its way in by needing to.

**Protect `obc_mode` with a checksum instead of triplication.** Tempting, since
the mechanism already exists for the persisted records. Rejected: a checksum
detects corruption but cannot repair it, and a system that discovers its mode is
untrustworthy has no safe answer available — nominal risks resuming a suspended
task, safe risks degrading on a corrupted bit. A voter produces an answer.
