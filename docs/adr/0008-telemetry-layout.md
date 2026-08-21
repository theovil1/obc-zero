# ADR 0008: One layout, and what telemetry owes the milestones before it

- **Status:** accepted
- **Date:** 2026-08-21
- **Milestone:** M6. Decisions 1–5 were taken before any of it was written;
  decision 6 was forced during implementation and is marked as such rather than
  presented as foresight.
- **Extends:** ADR 0004, ADR 0005, ADR 0007

## Context

M6 gives the system a way to describe itself: fixed-layout frames emitted on a
period, decoded by the host. Three earlier records left it obligations, one
acceptance criterion contains a design decision disguised as a requirement, and
one phrase — "a sensor returning garbage" — names three different failures that
are caught by three different means.

All of it is cheap to decide now and expensive to revisit once twenty fields
exist.

One more decision — which port the frames leave on — is recorded here as
decision 6. It was not foreseen. It was forced by implementation, and it is in
this record because it settles a boundary M7 inherits, not because it was
planned.

## Decision 1: the layout is read from the binary, not restated on the host

The criterion says the decoder and the flight code must take their field
definitions "from one source of truth, not two hand-maintained lists". That is a
requirement with several possible answers, and the wrong one is expensive.

**Rejected: a code generator.** A generator produces two artefacts from one
input and then needs a build step, a check that the generated files are current,
and a rule about whether they are committed. It replaces "two lists that can
drift" with "two files and a process that can drift", which is the same problem
with more moving parts.

**Chosen: a descriptor table in flash, read by the host from the ELF.**

The flight code carries a `const` table naming each field, its offset, its width
and its kind. The frame is packed from it. The decoder reads the same table out
of the binary under test and decodes accordingly. There is one definition, it
lives in flight code, and the host has no opinion of its own to drift.

This is the pattern already established here: M2's conformance checker derives
its expectations from the task table dumped out of the binary, precisely so that
a restated period on the host could not silently disagree with the flight one.
The same reasoning applies with more force to a wire format.

**The limit, stated because it will be raised.** This works because the host has
the ELF of the build under test. A ground station decoding a live downlink has
the same requirement — you fly a known build, and knowing which is a
configuration-management problem rather than a format one. If that ever stops
being true, the answer is to emit the layout's identity in the frame, not to
maintain a second list. Recorded so the question is reopened deliberately.

## Decision 2: telemetry becomes essential in safe mode

ADR 0005 defined safe mode as dispatching only tasks marked essential, and made
`housekeeping` the only one on the grounds that its continued dispatch is the
only evidence the executive is running. It also said M6 would revisit the set.

**It does, and the answer reverses.** A degraded system that stops describing
itself is a system nobody can diagnose, and safe mode is exactly the state in
which somebody most needs to know what happened. Suspending telemetry to save
work in a degraded system optimises the wrong thing: the work saved is small, and
what it costs is the only channel through which the degradation is visible.

So telemetry is marked essential. `scrub` and `audit` remain non-essential.

**The consequence for the static assertion**, which is not obvious: a boot is
called healthy after `OBC_HEALTHY_FRAMES`, defined as the period of the slowest
task so that every task has run once. Marking telemetry essential does not change
that, but adding a *slow* essential task later would — and the existing assertion
catches it.

## Decision 3: telemetry fills rung 2 of the escalation ladder

ADR 0007 declared the middle rung and left it empty, because a subsystem reset
needs a subsystem with its own state and none existed. **Telemetry is the first
one that does**: a sequence counter, a frame buffer, and per-sensor state.

Rung 2 therefore becomes: reinitialise the telemetry subsystem's own state and
continue, without resetting the machine. The ladder reads:

| Rung | Action |
|---|---|
| 1 | Suspend the offending task for its next due frame |
| 2 | Reinitialise the offending subsystem's state |
| 3 | Reset the machine |

**Rung 2 must be observable, on the same grounds rung 1 was.** A reinitialised
subsystem whose observable behaviour is unchanged is a rung that does nothing.
The sequence counter resetting to zero is the observable, and it appears in the
frame itself: a decoder seeing a sequence go backwards knows a subsystem reset
happened, and a test asserts that it did.

That is a real cost — a monotonically increasing sequence number is convenient,
and this breaks it. The alternative is a separate counter of subsystem resets,
which is another field and another thing to keep consistent. Breaking the
sequence is chosen because it is *impossible to ignore*: a decoder that does not
handle it fails loudly, where a decoder that ignores an extra field fails
silently.

## Decision 4: a lying sensor is flagged, and the flag is checked both ways

The criterion says a sensor returning garbage must be "flagged in telemetry, not
propagated". Both halves need an assertion, and the second is the one that gets
forgotten:

- **A flagged reading must not appear as a value.** The frame carries the flag
  and a defined substitute, never the garbage.
- **An unflagged reading must be a real one.** A frame with no flag set must
  contain a value the sensor actually produced.

The second direction is what stops the flag becoming decorative. A validator that
only checks "bad readings are flagged" passes on an implementation that flags
everything, and on one that flags nothing while the test happens to inject only
good values.

Plausibility is defined per sensor in the same descriptor table as the layout,
so the range a reading is checked against and the offset it is written to cannot
drift apart.

### Which lies are detected, and which are not

"A lying sensor" is three different failures that are detected by three different
means, and collapsing them lets the phrase quietly come to mean only the easy
one. Stated separately so that nobody has to infer which was meant:

| Failure | Detected at M6 | What it needs |
|---|---|---|
| Out of range | **yes** | a bound per sensor, held in the descriptor |
| Stuck on a plausible value | **yes** | memory of the last N readings |
| Oscillating between plausible values | **no** | a model of physical rate of change |

**Out of range** is a comparison and costs nothing.

**Stuck** costs memory: a reading that never changes is indistinguishable from a
stable measurement without a history to compare against. M6 keeps a small
per-sensor run-length — how many consecutive identical readings — and flags past
a threshold. That threshold is declared arbitrary: a genuinely constant quantity
would trip it, and the honest answer is that no sensor here measures one. It is
recorded as a figure to calibrate against a real sensor rather than derived.

**Oscillating between plausible values is not detected, and will not be at M6.**
Distinguishing a sensor flipping between two valid readings from a quantity that
genuinely changes quickly requires knowing how fast the measured thing *can*
change — a physical model, per sensor, that this project does not have and cannot
invent from an emulator. Writing a rate limit without one would produce a
threshold that flags real transients and misses real faults, chosen by whoever
typed it.

So the flag means "out of range or stuck". A campaign report saying "the sensor
lied and was caught" must say which of the three, and the third is an open gap
rather than a covered case.

## Decision 5: the trace stays where it is, and the weakness in that is named

The 256-byte execution trace has now been raised three times. It was decided at
M4, in ADR 0004: a permanent flight mechanism, not compiled out, because gating
it behind a build flag would mean the campaign measures an image with
`trace_push` in the dispatch path while the vehicle flies one without it — making
every published budget a figure about a binary that never flies.

That record left one sub-question open: the linear buffer is a placeholder and
becomes a ring "at M8, or earlier if M6 needs it for telemetry". **M6 does not
need it.** Frames are emitted and gone; nothing here looks backwards at dispatch
history. The commitment stands for M8 unchanged, and this is the resolution of
that clause rather than another deferral.

**But M6 exposes a weakness in the M4 argument that should be said out loud.**
Telemetry is flight observability with a flight channel. The trace is flight
code with no flight channel at all: it is read by a debugger, which means by
someone with physical access. Calling it a flight mechanism while only a
laboratory can see it is close to calling it instrumentation again.

The M4 reasoning survives — the budget argument does not depend on who reads the
buffer — but it is weaker than it looked. **M8 must resolve this by folding the
trace into the event log, which is downlinked.** If M8 finds a reason not to,
then ADR 0004's conclusion should be reopened rather than inherited, because a
flight mechanism nothing in flight can observe is a contradiction that has simply
not been paid for yet.

## Decision 6: the downlink is its own port

Decided during implementation, and it belongs here because it is a boundary
later milestones inherit.

The frames were first emitted on the console UART, beside the banner and the
safe-mode announcements. That worked and it broke three unrelated tests —
`test-wdt`, `test-trap` and `test-record` — none of which had anything to do with
telemetry. Their assertions read the serial log as text, and a log carrying
binary frames is not a text file: `grep` reports "binary file matches" and every
value-extracting assertion silently receives nothing. Three subsystems were
reported broken because a fourth had started speaking a different language on
their line.

**The fix is not to teach the harness to read binary.** That repairs `grep` and
leaves two unrelated streams sharing one pipe.

The machine models two UARTs. The console keeps UART0 and the downlink takes
UART1, and the reason is not the tooling:

- **They are different things.** The console exists because somebody is
  developing. The downlink is the system's product. A real vehicle does not
  carry its debug log on the link the ground decodes.
- **Sharing one line means writing a de-interleaver**, and that de-interleaver
  would end up in flight code — paid for permanently, to solve a problem created
  by a decision that could simply have been made differently.
- **M7 needs this boundary anyway.** A command uplink has to arrive somewhere,
  and that somewhere is not the text console. Separating now settles it once.

**UART1 was verified to be genuinely modelled before anything was built on it**,
by writing a byte through the debugger and reading it back from the host's
capture. A device that is present in the address map but discards writes would
have produced a passing test over a frame that was never emitted — a green
certifying nothing, which is the failure mode this project keeps finding.

## Consequences

**The frame is packed, not a struct.** A layout that depends on compiler padding
is a layout that changes with a compiler flag, and the size reference would
catch it as drift without saying why. Offsets come from the descriptor table and
the packing is explicit.

**No CCSDS or ECSS compliance**, per the milestone's own scope. What this
produces is a fixed binary layout with a known decoder, which is the thing a
standard would later constrain rather than replace. In the backlog, to be
revisited before any flight.

## The review question for this milestone

M5 produced three defects that were not bugs in any line: a watchdog that
depended on what it watched, a counter that assumed a path had been taken, a
suspension of a frame in which the task was not due. All three were mechanisms
that were correct and wrongly connected, and M5 was the first milestone adding
system properties rather than primitives.

**M6 is the first adding a subsystem.** The question to ask of every piece of it
is not "is this function correct" but **"what happens when the other part
fails"**:

- what does the decoder do when the frame is truncated, not when it is well
  formed;
- what does the frame contain when the sensor has not been read yet, not when it
  has;
- what does rung 2 reinitialise when the subsystem is already mid-reinitialise;
- what does the emitter do when the UART drops a byte, which it already can.

Each of those is a place where both halves work and the pair does not.

**It found one, and not one of the four.** The pair that failed was the frame
emitter and the *host's* text assertions — see decision 6. The list above was
worth writing and was not the answer; what made the difference was asking the
question at all, not guessing the four right places to ask it.

A second one, found and not fixed: telemetry's instruction budget holds only
because the emulated UART never stalls. `obc_uart_putc` retries a full FIFO up to
its bounded limit, so one stalled byte costs more than the whole task budget and
the executive would call it an overrun — escalating, and at rung 3 resetting the
machine because a downlink was congested. Both halves are correct alone. QEMU's
chardev accepts every byte immediately, so no campaign on this machine can
exercise it, and raising the budget to cover it would be tuning against a case
nothing can test. The fix is a non-blocking transmit path, which is a HAL change.
**In the backlog, not claimed as handled.**

## Alternatives considered

**Text telemetry, human-readable over the serial line.** Rejected: the system
already prints a human-readable banner, and adding a second textual channel
would leave the project with no binary format at all — which is what every later
milestone needs, and which is harder to introduce once a text format has users.

**A self-describing frame carrying its own field names.** Rejected on the memory
budget: names in every frame cost bytes per emission on a board with 16 KiB, to
solve a problem — the decoder not knowing the layout — that reading the binary
already solves.

**Keeping telemetry non-essential in safe mode.** Rejected above, and worth
naming as an alternative because it is the status quo and the cheaper option. It
loses the only view of a degraded system.
