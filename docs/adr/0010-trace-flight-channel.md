# ADR 0010: The condition under which ADR 0004 becomes false

- **Status:** accepted
- **Date:** 2026-08-21
- **Condition falls due:** when M8 delivers the event log, and in any case by
  **2026-10-01**, whichever comes first
- **Extends:** ADR 0004, ADR 0008

## Context

ADR 0004 decided that the 256-byte execution trace is flight code and is never
compiled out. The argument was about measurement: gating it behind a build flag
would mean the campaign measures an image with `trace_push` in the dispatch path
while the vehicle flies one without it, making every published budget a figure
about a binary that never flies.

That argument is sound and it is not the whole of the claim. M6 exposed the rest.

**The trace is flight code with no flight channel.** It is read by a debugger,
which means by someone with physical access to the board. M6 gave the system a
downlink; the trace does not use it. Calling something a flight mechanism while
only a laboratory can observe it is close to calling it instrumentation with
extra steps — which is the thing ADR 0004 set out to avoid.

ADR 0008 recorded this as a weakness and said M8 must fix it. That was a footnote
inside a record about something else, and a footnote is not a condition. This is
the condition.

## Decision

**M8 folds the execution trace into the event log, and the event log is
downlinked.**

If it does, ADR 0004's conclusion stands as written: the trace is flight code,
permanently compiled in, and it is observable from the ground like everything
else the vehicle knows about itself.

**If M8 does not, ADR 0004 is false and must be reopened rather than inherited.**
Not amended, not qualified — reopened, with the alternative it rejected put back
on the table:

- 256 bytes is **13 % of the RAM in use** at M6 (2036 B of 16384). Spending an
  eighth of the committed memory on a buffer nothing in flight can read is not a
  measurement argument, it is a subsidy.
- The measurement argument would then have to be met another way. The obvious
  candidate, and the one ADR 0004 rejected, is to keep `trace_push` compiled in
  and make the *buffer* the thing that shrinks — the call stays in the dispatch
  path, the budget stays honest, and the storage stops being 256 bytes of
  laboratory equipment.

## Why a date and not only a milestone

Milestones slip, and a condition attached only to one that slips is a condition
that never falls due. **2026-10-01** is a backstop: on that date the question is
asked whether or not M8 has started.

The date is arbitrary and declared so, on the same footing as
`OBC_SHORT_BOOT_LIMIT` and `OBC_SENSOR_STUCK_LIMIT`. What is not arbitrary is
that there is one.

## How it is checked

By reading this record, which is the honest answer — nothing automated can decide
whether a buffer is downlinked. What *can* be automated is narrower and is worth
having: M8's own acceptance must include an assertion that trace content reaches
the ground through a frame, not through a debugger. If M8 ships without that
assertion, this condition has not been met regardless of what the code does.

## Consequences

**This record is the reason ADR 0004 is not quietly wrong.** An architecture
decision that is 90 % right and inherited forever is more expensive than one that
is wrong and revisited, because the inherited one stops being examined. Writing
the condition down with a date is what keeps it examined.

**It also sets a precedent worth naming.** When a later record finds a weakness
in an earlier one, the weakness gets its own dated condition rather than a
sentence inside the later record. ADR 0008 tried the sentence first, and the
sentence is why this had to be written separately three hours afterwards.
