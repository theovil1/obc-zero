# ADR 0004: The execution trace is flight code, not instrumentation

- **Status:** accepted
- **Date:** 2026-08-21
- **Milestone:** decided at the end of M2, binding on M4, M6 and M8
- **Extends:** ADR 0002 and ADR 0003

## Context

M2 put a 256-byte execution trace in RAM — 256 of the 384 bytes the scheduler
consumes, and 1.6 % of all the memory this board will ever have. Today only the
host test reads it.

That invites a fair challenge: this looks like test instrumentation living in a
flight image, and ADR 0001 already took the opposite position on semihosting,
insisting that a host exit gate must never exist in a binary intended to fly.
If that rule is right, why is this different?

The question has to be settled before M4. `docs/BUDGET.md` is a register of
claims about the memory of the system that flies; if part of what it measures is
compiled out of the real image, it is measuring something else.

## The semihosting analogy does not hold, and the difference is the point

Semihosting is a **call into the host**. It functions only because an emulator
is answering on the other side. On silicon the same instruction sequence is an
illegal instruction or, worse, a live channel to whatever debug probe is
attached. It is a mechanism whose behaviour depends on the environment, and one
of those environments is a vehicle in flight.

A buffer in RAM is none of that. It behaves identically on QEMU and on an FE310,
it grants nothing to anyone, and reading it requires the same physical access
that reading any other RAM would. It is not a gate.

So the rule that bans semihosting does not reach this, and importing it here
would be reasoning by surface resemblance.

## The decisive argument is the project's own

Suppose the trace were put behind a build flag. Then:

- the campaign measures image A, with `trace_push` in the dispatch path;
- the vehicle flies image B, without it;
- every execution budget in `docs/BUDGET.md` and every instruction count in the
  logbook belongs to A.

That is precisely the argument ADR 0002 used to justify applying `-icount` to
every invocation rather than to campaigns alone: *a test that runs under
different semantics from the campaign is not testing the campaign*. A build flag
recreates the same defect one level up, and it recreates it in the numbers the
project publishes as evidence.

Keeping the trace unconditional costs 256 bytes. Removing it costs the meaning
of every budget figure measured so far.

## Decision

**The execution trace is a permanent flight mechanism.** It is not compiled out,
not gated behind a flag, and `docs/BUDGET.md` therefore already measures the
image intended to fly. No dual-configuration size reference is needed.

**But its current shape is a test shape and must not be grandfathered.** A
linear buffer that stops recording after 256 dispatches is right for a 16-frame
assertion window and useless in flight, where frames run indefinitely: it would
fill within seconds and then record nothing for the rest of the mission. A
mechanism that is dead weight after the first minute is not flight
observability, it is instrumentation wearing its badge.

**The trace becomes a ring buffer with an explicit wrap counter**, holding the
recent past rather than the distant beginning. Delivered with the event log at
M8, or earlier if M6 needs it for telemetry; until then the linear buffer is a
placeholder and is labelled as one in `flight/core/sched.h`.

The ordering assertion survives the change without being weakened. M2 rejected a
ring precisely because wrapping would silently change what the assertion
compares — that objection is answered by making the wrap **explicit** rather than
by avoiding the ring:

- the assertion window is sized to fit inside the buffer, so a test run never
  wraps;
- the checker requires the wrap counter to be zero, because a wrapped window
  cannot be compared position by position and a checker that tried would be
  comparing a rotation;
- in flight a non-zero wrap counter is normal and is itself a reportable figure.

## Consequences

**`docs/BUDGET.md` needs no second configuration.** The numbers it holds are the
flight numbers. This was the concrete risk the question raised and it is closed.

**The 256 bytes are now a design commitment, not an accident.** They must be
justified against M8's event log rather than sitting beside it: two independent
records of what recently happened would be a duplication that the 16 KiB cannot
afford. M8 must either absorb the trace or explain why it is separate.

**The rule this establishes, stated so it can be applied without re-arguing:**
a mechanism belongs in the flight image if it behaves identically on silicon and
grants no capability that depends on the environment. A mechanism that only
functions because a host is listening does not, whatever it is called. The test
is not whether a feature is *used* in flight but whether its behaviour is
*defined* there.

## Alternatives considered

**Build flag, with the size reference tracking both configurations.** Rejected
for the reason above: it makes every published budget a figure about an image
that does not fly. The bookkeeping is the smaller half of the cost.

**Drop the trace and assert only the per-task counters.** Rejected: counters
give conformance but not order, and M2 found that a dropped dispatch shows up in
both. Order is half the property and it is the half a counter cannot express.

**Keep the linear buffer as it is and call it flight code.** Rejected. It would
be true in the letter and false in substance: a buffer that is full after the
first minute of a mission observes nothing for the rest of it, and declaring it
flight observability would put a claim in `BUDGET.md` that the mechanism cannot
support.
