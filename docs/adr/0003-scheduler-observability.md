# ADR 0003: What the scheduler must prove, and what it will not enforce

- **Status:** accepted
- **Date:** 2026-08-21
- **Milestone:** decided before M2 is written
- **Extends:** ADR 0002, which fixed the two time domains

## Context

Everything built so far fails loudly. A trap fault stops the machine. A carry
defect shows up as a counter off by thirty-six hours. The footprint guard fires
on four bytes. In each case the failure announces itself.

The scheduler is the first component where that stops being true. **A scheduler
can be subtly wrong and run correctly for months.** A task that runs 999 times
instead of 1000 in a window, a period that drifts by one frame, an ordering that
inverts under a particular arrival pattern — none of these stop anything. They
produce a system that works, until the one situation where the missing execution
was the one that mattered.

So the property has to be written before the code, and it has to be stronger
than "the tasks run".

## Decision 1: the property is exact trace equality, not a jitter bound

**What M2 must prove is not that tasks execute. It is that the order and the
number of executions within a given window are exactly those the task table
dictates, and that they do not vary between runs.**

The scheduler emits an execution trace: for each dispatch, the task identity and
the `minstret` value at entry and exit. The assertions are:

1. **Conformance.** Over a window of N frames, each task's execution count
   equals `N × frame_period / task_period` exactly. Not approximately, not
   within a tolerance — exactly, because a cyclic executive with a static table
   has no source of legitimate variation.
2. **Order.** The sequence of task identities matches the sequence the table
   defines, position by position.
3. **Reproducibility.** Two runs of the same image produce **byte-identical
   traces**.

The third is the one worth the effort. Under `-icount` execution is
deterministic, so identical traces are not a hope but a requirement, and any
difference between two runs is a defect by definition — there is no
non-determinism left to blame it on. That is a far stronger assertion than a
jitter bound, which merely says the variation stayed inside a number somebody
chose.

A jitter bound also has a failure mode this avoids: it passes on a scheduler
that is consistently wrong. Trace equality does not care whether the timing is
good, only whether it is *the timing the table specifies*, and it compares
against the specification rather than against a tolerance.

**Consequence for the tolerance question:** there is no tolerance to pick. This
removes an entire category of argument about what value is acceptable, which is
usually a sign the property was chosen correctly.

## Decision 2: M2 observes budget overruns; it does not prevent them

Budgets are denominated in retired instructions, per ADR 0002, because at
32768 Hz one tick is 30.5 µs and a task that runs in a few microseconds measures
zero or one tick.

That choice has a consequence which has to be faced rather than discovered:
**an overrun is detected when the task returns, which is after it has already
overrun.** Reading `minstret` at exit and comparing to the budget tells you what
happened; it cannot tell you while it is happening.

Preventing an overrun needs a mechanism that interrupts a running task:

- a timer interrupt armed on `mtimecmp` at the budget boundary — but the budget
  is in instructions and `mtimecmp` is in ticks, so the boundary is
  approximate, and at 30.5 µs resolution it is very approximate for a task
  measured in microseconds;
- a context switch out of a task that did not yield, which turns a cooperative
  executive into a preemptive one and brings in register save and restore, a
  per-task stack, and re-entrancy questions across the whole flight code;
- an interrupting handler that saves context — which sits directly against the
  M1 finding that the trap handler must not touch the stack.

**Decision: M2 detects, records and counts overruns. It does not prevent them.**

This is a choice, not an oversight, and the reasoning is:

- An overrun in a system with a static task table and no dynamic allocation is a
  **design defect, not a runtime condition**. The correct response is to find it
  in a campaign and fix the task, not to paper over it at run time.
- Detection is sufficient to make it visible: the overrun is counted, reported
  in telemetry, and a campaign that produces one fails.
- Preemption is a large amount of machinery, and every piece of it becomes new
  surface for the fault campaigns to attack. Adding it to *avoid* a class of
  defect that detection already surfaces is a poor trade at this stage.
- The M1 no-stack constraint on handlers is a property worth keeping. A
  preemptive design would have to relax or complicate it.

**What this decision costs, stated plainly:** a task that overruns badly enough
to miss the next frame boundary will cause a frame overrun that the scheduler
observes but did not stop. The system degrades in a visible, recorded way rather
than a silent one. That is acceptable for a cyclic executive whose task set is
fixed at compile time; it would not be acceptable for a system running
unpredictable work, and this one never will.

**Revisit if:** M5's escalation ladder needs to bound the damage of a runaway
task rather than merely record it, or if a real mission profile introduces work
whose duration is not known at build time. Either would be a reason to reopen
this record with a new one, not to quietly add preemption.

## Consequences

**The task table is a compile-time constant and is verified as one.** Static
assertions on periods, counts and budgets, so a table that cannot possibly meet
its own frame is a build failure rather than a campaign result.

**The trace is a first-class output, not debug printing.** It is what the
assertions compare, so its format is fixed and versioned like the size
reference, and a change to it is a deliberate act.

**The trace must not perturb what it measures.** Emitting it over the UART
inside a dispatch would add instructions to the very count being asserted. It is
therefore written to a fixed-size buffer in RAM and read out by the host through
the debugger, which costs the guest nothing while it runs. The buffer competes
for the 16 KiB in `docs/BUDGET.md` and must be sized within the M2 line.

**Idle time is measured, not assumed.** The slack between the end of the last
dispatch and the frame boundary is recorded in the same instruction domain. A
frame with no slack is a frame that is about to overrun.
