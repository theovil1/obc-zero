# Recovery code is almost never wrong in the direction you test it

Six months of evenings on a bare-metal flight software core produced a handful of
defects worth writing about. Not because they were hard to fix — most were three
lines — but because every one of them shared a property I did not expect, and
which I have not seen stated plainly anywhere.

**A recovery mechanism is rarely broken. It is usually correct, and connected to
the system in a way that means it never runs, or runs without effect, or reports
something that did not happen.**

Each of those is invisible to the test you would naturally write, because the
code does exactly what it says and the system behaves exactly as designed. The
only thing missing is the part nobody thought to observe.

What follows is six cases, grouped by how they hide rather than by the order I
found them. The context is a small RISC-V system — 16 KiB of RAM, a cyclic
executive, no operating system — but nothing below depends on that. If you write
firmware, drivers, supervisors, or anything with an error path you cannot easily
reach, the shapes should be familiar.

---

## Group one: the mechanism only runs where you are not looking

### The trap handler that worked until it was needed twice

On RISC-V, a machine-mode trap handler that wants to save registers has a
problem: it has no stack it can trust. If the fault being handled *was* a stack
overflow or a corrupted stack pointer, pushing a frame faults again — and the
second trap overwrites `mepc` and `mcause`. The original cause is destroyed and
only the handler's own fault survives.

The standard answer is `mscratch`, a scratch CSR, used like this:

```asm
csrrw t0, mscratch, t0    # t0 = record pointer, mscratch = interrupted t0
```

One instruction, no stack, and `t0` is recoverable. This idiom is correct. It is
in every reference. It relies on the handler swapping back before `mret`.

**My handler never returns. It records the fault and resets.**

So it never swapped back, and `mscratch` was left holding the interrupted `t0`.
A *nested* trap then swapped that garbage into `t0` and wrote its marker to an
arbitrary address.

Consider what passed while that was true:

- every nominal boot, hundreds of them;
- a fault injected with a valid stack: correct cause recorded;
- a fault injected with the stack pointer set outside RAM entirely: still the
  correct cause, because the handler genuinely does not touch the stack.

Two fault-injection tests, both green, both testing the handler. The defect lived
only in the path taken when the handler *itself* fails — which requires two
failures in a row, and which no amount of nominal testing reaches.

Code review would not have found it either. The idiom is textbook-correct and
wrong here for a reason that depends on a property of *this* handler: that it
resets rather than returns. You would have to be holding both facts at once.

It was found because a test existed whose only job was to make the system fail
while it was already failing.

### The watchdog that depended on what it watched

Later, the same shape at system level.

A software watchdog is fed when all scheduled tasks have run, and something has
to check it and escalate. The obvious place to put that something is a task —
you already have a scheduler, tasks are how work gets done, and a watchdog task
is a normal design.

It is also a watchdog that is fed by, checked by, and escalated by the very
executive it exists to watch. If the executive fails, the watchdog fails with it.
What remains is a mechanism that works in precisely the cases where everything
else already does.

The fix is structural rather than clever: the check lives in the frame loop, and
the backstop underneath it is the hardware watchdog in the always-on power
domain, which does not care whether any software is running.

The test that matters follows from this. **Hang the executive, not a task.** A
hung task is caught by the budget check, which runs inside the executive — so a
watchdog test that hangs a task proves the watchdog against the case that never
needed it. I park the program counter in a genuine infinite loop that exists in
the real firmware, mid-window, and assert the machine resets on its own.

### The counter that assumed a path had been taken

That test immediately found a third instance.

Reset-loop protection counts consecutive failed boots. My first version
incremented the counter on the reset path, just before resetting — which is
correct, and is what you would write.

Then the hardware watchdog fired. It resets the machine **in hardware, with no
software running at all.** No path was taken. Nothing incremented anything. The
counter stayed at zero and the system would have looped forever without ever
noticing — the precise failure the protection exists to catch, arriving through
the one door a software counter cannot cover.

The correction is a change of what is being measured. Instead of counting the
taking of a path, raise a flag as early in the boot as possible and lower it only
on success. A boot that finds the flag still raised knows the previous one died,
**however it died and however early**. It measures the absence of a success
rather than the presence of a failure, and absence has no path to miss.

There is an irreducible window — a boot dying before the flag is raised is not
counted — and it is worth writing that down rather than discovering it later.

---

## Group two: the mechanism runs, and has no effect

The first group hides by not executing. The second is worse, because it executes
and everything looks right.

### The scheduler that never waited

A cyclic executive runs a fixed table of tasks in fixed frames. Mine was
asserted hard: over a sixteen-frame window, each task's execution count must
equal exactly what the table dictates, the order must match position by position,
and two runs of the same image must produce identical traces.

All of that passed. Then a number did not add up — the run took 0.2 seconds of
host time where the arithmetic suggested 2.5 — and chasing it turned up that
**nothing asserted the window had taken the time it should have.**

An executive could dispatch the right tasks, in the right order, record healthy
slack in every frame, and never actually wait out a single frame boundary. Every
assertion I had written would pass. It would be correct in everything the trace
can see, and wrong about time, which is the one thing a scheduler is for.

The window's start and end are now recorded and the span asserted exactly. The
discrepancy turned out to be my own bad estimate of emulation speed — but the
assertion it produced is one no amount of reasoning about the design would have
suggested, and it exists because a number was not rounded away.

### The suspension that suspended nothing

The recovery ladder's first rung suspends a misbehaving task for one frame. I
implemented it as "do not dispatch this task in frame + 1".

For a task that runs every frame, that withholds a dispatch. For a task that
runs every second frame, **frame + 1 is a frame in which it was never going to
run.** The suspension suspends nothing. The counter increments, the log records
it, the system behaves exactly as designed, and the task runs on schedule.

This one is instructive because of how it was found. The host-side checker had
to learn about suspensions, or it would report every withheld dispatch as a
dropped one — a false failure. Writing that exemption, I also wrote its mirror:
an announcement with no gap behind it is as much a defect as a gap with no
announcement.

The mirror fired on the first run.

I would not have found it otherwise. When you write an exemption you are
thinking about the false failure you are trying to prevent, and that direction
gets your attention. The other direction — *did the thing actually happen* — is
the one you write only if you decide, as a rule, always to write it.

### The scrubber that has never scrubbed

Included because it is unresolved, and because the honest version of this list
has to contain one.

Critical state is stored in triplicate and read through a voter that repairs a
dissenting copy. A periodic scrubber walks the copies and repairs them without
anyone needing to read the value.

Its cost is measured: 221 instructions of a 3000-instruction budget. It has never
repaired anything. The only critical state I have is read several times per
frame, so read-repair always reaches a corruption first — measured directly: the
repair counter is already at 1 by the time the scrubber is first called.

For one hot variable, the scrubber is redundant. It stops being redundant the
moment there is state written once and read rarely, where a single corruption
can wait hours for its next read while a second one arrives and turns a
recoverable vote into an unrecoverable one.

So the acceptance criterion stays unticked. **Its cost is measured; it has never
been shown to work.** Ticking it would be claiming a mechanism was validated when
only its price was known, and those are not the same claim.

---

## Group three: the instrument lies

The first two groups are about the system. This one is about the thing measuring
it, and it produced the most useful lesson.

### Four green runs on faults that never happened

Four separate times, a test reported success while injecting nothing.

- A debugger script hit a syntax error on its second line. The script aborted,
  the firmware ran undisturbed, the expected output appeared, and the run was
  green — on an injection that had never occurred.
- An injection landed on the baseline reading that the comparison was made
  against, so the baseline moved with everything else and the observed delta was
  zero. Green.
- A fault threshold was set to 200,000 operations. A full run performs 166,817.
  The threshold was beyond the reach of the entire run. The fault never fired.
  Green.
- A source override was applied to the build but not to the debugger, so the
  right binary was tested by a script pointed at the wrong source.

Four different causes. That is what makes it a pattern rather than carelessness:
there is no single mistake to stop making.

The generalisation that came out of it is the one I would keep if I kept nothing
else from this project:

> **An injector is a program, and its constants are part of it.** A threshold, a
> seed, a breakpoint count — each is measured against the system under test, not
> estimated. And every injector must emit positive proof that the fault landed,
> with the run failing when that proof is absent. A return code says the tool
> exited. It says nothing about whether the tool did anything.

A test that reports success on a fault that never occurred is worse than no test,
because it is counted as coverage.

### One red run on six subsystems that were fine

Then the opposite, once.

Running a long campaign alongside the ordinary test suite produced six confident,
specific failure reports — naming the scheduler, the trap handler, the persisted
record, the degraded mode. Every one of those subsystems was working perfectly.

Every injector hardcoded the same debugger port. The second emulator failed to
bind, the debugger attached to the first one's target, and the failures described
a system nobody was testing.

**The false red is the more expensive of the two, and it is not close.**

A false green costs a missed defect, once, and the defect is still there to be
found later. A false red costs something that does not repair: it teaches you not
to believe failures. Once the habit forms of re-running a red test because *it is
probably the environment*, the test suite has stopped being evidence and become a
suggestion. There is no fix for that in the suite, because the damage is to the
person reading it.

Which is why a harness may fail, and may not be confidently wrong. The port is
now allocated per run, and a collision reports a collision.

### The tool that destroyed the evidence

The last one is mine in a way the others are not.

The project has a rule, written early and applied since: campaign reports are
append-only history. Superseded by newer ones, never edited, never deleted. It
exists because the reports are the only thing the project actually produces.

I wrote a campaign runner. It named its report by date. Running the campaign
twice on the same day **overwrote the first report**, silently. The only reason
the earlier one survives is that it happened to have been committed in between —
an accident of sequencing, not a property of anything.

The rule was written imagining a person editing a file. What broke it was a
program doing exactly what it was told.

That case is worse than the other five, for a reason that took me a while to
state: **it is the only one whose consequence is unrecoverable.** A false green
costs a defect found later. A hardcoded port costs an afternoon. A mechanism that
never runs can be connected properly. All of those are repaired by doing more
work. An overwritten result is not — the run that produced it happened against a
commit that no longer builds the same binary, and no amount of later effort
reconstructs it.

If you keep one prioritisation rule from this: **controls that guard evidence
come before controls that guard anything else.**

---

## What I would apply elsewhere

Four rules, in the order I would adopt them.

**1. When an assertion has two directions, write the one that observes an effect
first.**

Every defect in group two was invisible to a naturally-written test and visible
to its mirror. The pattern is consistent enough to be mechanical: the direction
that gets forgotten is the one requiring you to *see something happen*, because
the other direction — no problem occurred — is cheaper to write and feels like
coverage.

Assert that the gap exists **and** that the announcement exists. That the counter
moved **and** that the thing it counts happened. That the mechanism was invoked
**and** that its effect is visible from outside.

**2. Test the recovery path by failing while already failing.**

Not by failing normally. The mechanism you are testing exists for the case where
something has already gone wrong, so a test that starts from a healthy system
exercises the easy half. Hang the supervisor, not the supervised. Fault inside
the handler, not before it. Corrupt the thing the repair mechanism reads.

**3. A recovery path tested by a single injection proves that injection site,
not the path.**

If a mechanism has three ways in, exercise all three. Mine had a criterion
saying "reachable from three subsystems" that would have been ticked on an
implementation where one of those entries was wired to one of three call sites —
the injected failure landed on another, and the system returned an error without
recovering at all. Where a voter has three copies, corrupt each one; they are not
symmetric in the code even when they are symmetric in the design.

**4. A check that can detect a violation must refuse to proceed.**

I built four mechanisms correctly and shipped each without a refusal, and
violated every one of them myself:

| Mechanism | What it did | What it needed to do |
|---|---|---|
| Reference figures | asked for a justification in the commit message | refuse without one |
| Debugger port | assumed it owned a fixed port | allocate per run, check first |
| Report writer | overwrote yesterday's report | refuse to overwrite |
| Content audit | printed a warning, commit proceeded | block the commit |

A warning is not a control. And a refusal that arrives after the expensive work
is a degraded refusal — my report writer checked its output path *after* running
a fifteen-minute campaign. Moved to the start, it declines in 0.04 seconds.

---

## Status

This is a pre-alpha hobby project. Nothing in it has flown, nothing is qualified,
and every measurement above was taken under emulation — which models no
radiation, no power rails and no thermal behaviour, and is therefore a lower
bound on real fault rates and never an upper one.

The measurements, the seeds and the commands to reproduce them are in the
repository's reports, each naming the commit it was taken against. The defects
described here are in its history, and the reasoning behind each fix is in its
decision records and its logbook rather than summarised from memory.

I am publishing the failures because the failures are the part I could not have
read anywhere.
