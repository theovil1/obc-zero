# ADR 0009: What a subsystem does when it cannot do its job

- **Status:** accepted
- **Date:** 2026-08-21
- **Milestone:** between M6 and M7, before the command path repeats the question
- **Extends:** ADR 0002 (time domains), ADR 0007 (escalation), ADR 0008 (telemetry)

## Context

M6 shipped with a reservation: the telemetry task's instruction budget holds only
because the emulated UART never stalls. That was recorded as a backlog item, and
recording it was the wrong response.

**A green test on a path the emulator cannot exercise does not say the path is
fine. It says the path was not tested.** The distinction is the whole difference
between evidence and its absence, and this project has already decided which of
those it produces.

Three facts were measured before anything here was decided.

### Fact 1: the retry bound is per byte, and nobody derived it

`obc_uart_putc` retries a full transmit FIFO up to `UART_TX_RETRY_LIMIT` times.
That constant is 100000. From the disassembly of the shipped binary, the poll
loop is four instructions:

```
lw   a6,0(a2)     ; read txdata
bltz a6,+8        ; bit 31 set means full
addi a5,a5,-1     ; spend one retry
bnez a5,-16       ; go round again
```

and the counter is reloaded for **each byte**. One stalled byte therefore costs
up to 400000 instructions, against a task budget of 3000 and a whole-frame
capacity of 487424. A single congested byte consumes 82 % of the frame every
other task shares.

**The number 100000 is not derived from anything.** It is a large round number,
and the loop it bounds is inside a task governed by a budget it has never heard
of. That is the defect, and the UART is only where it happens to live.

### Fact 2: the measured budget is an emulation artefact

The telemetry task measures 1466 instructions per dispatch. On silicon, at the
115200 baud the driver configures:

| | Instructions |
|---|---:|
| One byte on the wire, at 15.625 MHz | 1356 |
| Poll iterations to cover one byte | 271 |
| A 45-byte frame | **61035** |
| Frame capacity | 487424 |
| Share of one frame | **12.5 %** |
| Declared budget | 3000 |

**The real figure is twenty times the budget.** The 1466 does not measure
telemetry; it measures a UART with no baud rate, because QEMU's chardev accepts
every byte the instant it is offered. ADR 0002 already requires the port to
recalibrate its time domains — this is the first concrete instance, and it is
larger than a calibration factor. It is a design that does not fit.

### Fact 3: the stall is not injectable at the register on this machine

QEMU's `sifive_uart` does model an eight-entry transmit FIFO and does set the
full bit — verified by filling it through the debugger and reading the bit back.
But the FIFO drains on a bottom half that runs the moment the vCPU resumes:
filled while halted, the bit is set; **after a single retired instruction it is
clear again.** Measured with `stepi`, not assumed.

So a sustained stall cannot be produced by writing device registers here. That is
a property of the machine model, and it is stated rather than worked around,
because "we injected a stall" would otherwise mean "we injected one instruction
of stall".

## Decision 1: observability loses its data before the system loses itself

The question the M6 reservation actually posed: when a subsystem cannot do its
job, does it **abandon the work and record that it did**, or does it **spend its
budget trying and let the escalation ladder deal with it**?

For telemetry the answer is abandon, and the reason generalises:

> **A subsystem whose purpose is observability sheds its data rather than the
> system. A subsystem whose purpose is control does the opposite.**

Telemetry exists to make the system visible. A mechanism built to make the system
visible must not be able to take the system down — and spending the budget is
exactly that: overrun, rung 1, rung 2, rung 3, a machine reset caused by a
congested downlink. The reset does not drain the FIFO. It costs a boot, the
uptime, and the volatile state, and then the downlink is still congested.

A dropped frame costs one frame. There will be another in 62 milliseconds.

**This is the first time a flight subsystem here has had to choose**, and M7's
command path will pose it again with the opposite answer: a command that cannot
be validated is not shed quietly, because a command is control and losing one
silently is losing authority over the vehicle.

## Decision 2: the bound comes from the budget, and the build checks it

The retry allowance is no longer a constant somebody typed. It is:

- **one allowance for the whole frame**, not one per byte, so the worst case is
  bounded by the frame and not by the frame length times a constant;
- **sized from the task budget minus the measured nominal cost**, divided by the
  measured cost of one poll iteration;
- **asserted at compile time**, where the budget and the allowance are both
  visible. Lower the budget or raise the allowance until they no longer fit and
  the build fails rather than the vehicle.

The three numbers that feed it — nominal dispatch cost, poll loop cost, budget —
are each measured and each cited where they are used. None of them is round.

**And the poll cost changed when the bound did.** Moving from a per-byte counter
to a shared allowance puts the exhaustion test inside the loop, so the body went
from four instructions to five. The first version of this record carried the four
over from the earlier disassembly and was wrong by one for exactly as long as
nobody re-derived it — the same failure as the constant it replaced, one order of
magnitude smaller. The figures here are 5 instructions per poll, an allowance of
256, and a measured nominal of 1466: 1466 + 1280 = 2746 against a budget of 3000.

## Decision 3: a dropped frame is counted, and the count is published

Following the rule this repository keeps relearning: **assert the effect, not the
absence of a problem.** A drop that is merely "not emitted" is indistinguishable
from a frame that never came due, and the two need different responses from the
ground.

So a drop increments a counter, and the counter is a field in the frame. The
first frame that does get out says how many did not. A drop also announces itself
on the console, which is the channel that does not depend on the downlink
working.

**The counter is not reset by a rung-2 subsystem reset.** Rung 2 returns the
subsystem's own state to a known value; erasing the record of why it was needed
would leave a campaign with nothing to read.

## Decision 4: the emulated budget is labelled, not trusted

`T1_BUDGET` stays at a figure derived from the emulated cost, because that is the
machine the campaigns run on and a budget that no run can meet would make every
campaign red for a reason no campaign can fix.

**But it is labelled in the table as an emulation figure with the silicon figure
beside it**, so that nobody ports this and discovers at integration that
telemetry needs twenty times its declared budget. The port has a number to start
from and a statement that the current design does not fit it.

**What the port has to change is not the budget.** Polling 61035 instructions
inside a dispatch is not a budget problem, it is the wrong shape: the transmit
has to become non-blocking, with the frame handed to a driver that drains it
across frames. That is a HAL change and it is out of scope here — but it is now
recorded as a design conclusion rather than as a number to be raised.

## Consequences

**The stall is exercised through a HAL substitute, and what that proves is
stated.** `harness/broken/uart_stalled.c` reads the downlink's status from a word
the harness owns instead of from the device register, and **chooses that source
outside the poll loop** so the loop it runs is instruction for instruction the
flight build's.

That detail is the whole of it, and it was learned the hard way. The first
version counted down inside the loop, adding about five instructions to a loop
the flight build runs in five: a stalled dispatch measured 3949 instructions
instead of 2746, overran its 3000 budget, climbed the ladder and **reset the
machine** — the precise outcome this record forbids, produced by the instrument
rather than by the code, while the test reported PASS because nothing asserted
that the system had survived. Two defects in one run: an instrument that
perturbed past the threshold it was measuring, and a check that never looked at
the outcome that mattered. It proves the policy — the frame is abandoned, the drop is counted and
published, the task stays inside its budget — and it does **not** prove QEMU's
device path, which fact 3 says cannot hold a stall. Both halves are said out
loud, because "we tested the stall" would otherwise cover a substitution that
never touches the device.

This is the technique already used for `mtime_unstable.c` and for the same
reason: a fault the machine will not produce is still a fault the code must
survive.

**What the suite asserts, and what it does not.** With the port refusing, the run
must boot exactly once, the executive must judge no dispatch over budget, the
shed frames must be counted and reconciled against the sequence the ground
decodes, and the refused dispatch must cost at least `allowance x poll` more than
an accepted one. That last is a floor and not an equality: the two paths differ
in more than their polling — the refused one announces and skips forty-five byte
writes — and predicting the net would be predicting the compiler. A build that
gives up on the first refusal is caught by it, and `harness/broken/uart_impatient.c`
proves the floor does that work.

**The per-byte allowance is not testable this way, and is not claimed to be.**
Against a port refusing from the first byte, a per-byte emitter abandons exactly
as early as a per-frame one and costs the same; they diverge only on a port that
refuses intermittently, which this machine cannot produce without instrumentation
costing more than the thing measured. What contains it is the single allowance
declared outside the byte loop — one `li a4,256` hoisted above it in the
disassembly — and the compile-time assertion. Said here so nobody reads the green
suite as covering it.

**The frame grows by two bytes** for the drop counter. The host reads the layout
from the binary, so nothing on the ground needs changing — which is the first
time that property has been worth anything, and the reason it was built.

**The console keeps a per-byte bound**, now the same derived figure rather than
100000. Announcements from inside a dispatch — the escalation ladder's, above
all — sit in the same position telemetry was in, and leaving one of the two fixed
would be fixing the instance rather than the defect.

## Alternatives considered

**Raise the telemetry budget to cover a stall.** Rejected: it tunes a threshold
against a case nothing on this machine can produce, and the figure it would need
on silicon — 61035 — says the design is wrong rather than the budget is small.

**Escalate on a stalled port.** Rejected under decision 1, and worth stating
plainly: it is the option that reads as rigorous. A downlink that will not drain
*is* a fault, and escalating *is* what the ladder is for. It is wrong because the
ladder's top rung resets a machine whose only problem is that nobody is listening
to it.

**Drop silently and let the sequence gap show it.** Rejected: a sequence gap is
also what a lost frame on the link looks like, and the ground cannot tell a
vehicle that chose not to send from a frame that did not arrive. Those need
different responses.
