# Bringing up a fault-tolerant RISC-V flight core, and the five things that were wrong

- **Date:** 2026-08-20
- **Milestones:** M0 (boot) and M1 (time and traps)
- **Commit:** `76f7ca8c03fd7536db6145d7a6aa1fbd59e1e317`
- **Toolchain:** GCC 14.2.0, QEMU 10.2.1, `sifive_e`, RV32IMAC
- **Status:** pre-alpha. Nothing here has flown, nothing here is qualified.

## Why this report exists

OBC-Zero is a bare-metal flight software core whose deliverable is not features
but measured fault tolerance. It is not far along: it boots, it keeps time, and
it survives a fault without lying about what happened. Two milestones.

This report is not a tutorial. Tutorials describe a path that worked, which is
the least informative version of any engineering story. What follows is the list
of things that were **wrong**, most of them wrong in ways that passed their
tests, because that is the part worth reading and the part nobody publishes.

Five findings. The first three are defects in the *test infrastructure* — the
part everyone trusts implicitly — and the most serious of them made a test
report PASS on a fault that had never been injected.

---

## 1. Three tests that passed without testing anything

The milestone required a specific defect to be provoked: on RV32 the 64-bit
machine timer is read through two 32-bit registers, and a naive read straddling
the low-word carry returns a value roughly 2^32 wrong. Waiting for that carry is
not viable — at 32768 Hz it arrives once every 36 hours — so it is forced
through the debugger while the reader sits between its two loads.

The harness that forces it was wrong three times, and each time it reported
success.

**The debugger script failed and the run stayed green.** `break $carry_line` is
rejected by GDB, which requires an integer for a line-spec convenience variable.
The script aborted at that line. The firmware then ran completely undisturbed,
printed its normal sentinel, and the run was reported PASS — on an injection
that had never happened.

The fix is not the syntax. It is that **the run now demands positive proof the
fault landed**: the script emits `CARRY-INJECTED` after writing guest state, and
the test fails if that string is absent. A return code says the tool exited. It
says nothing about whether the tool did anything.

**The injection landed on the wrong read.** Corrected syntax, injection
confirmed — and the observed delta was zero. The carry had been forced into the
*baseline* reading that the comparison is made against, so the baseline moved
along with everything else and nothing was exercised. A green run, again.

**A source override applied to the build but not to the debugger.** The
deliberately-broken build variant compiled correctly, but the debugger was still
pointed at the flight source. The breakpoint resolved against a file absent from
the binary, and the run hung instead of failing. A hang is at least honest; the
first two were not.

The general rule that came out of this, now written into the milestone that
builds the real harness: **every injector asserts that its fault landed, and the
run fails without that assertion.** A test that reports PASS on a fault that
never occurred is worse than no test, because it is counted as coverage.

---

## 2. A defect that only exists after everything else has failed

This is the finding worth the whole exercise.

The trap handler must not touch the stack. If the fault being handled *was* a
stack overflow or a corrupted stack pointer, then saving a context frame faults
again immediately — and on RISC-V the second trap overwrites `mepc` and
`mcause`. The original cause is destroyed and only the handler's own fault
survives. The evidence the handler exists to capture is precisely what a
stack-using handler loses.

So the handler reaches its record through `mscratch` and never touches `sp`.
The standard idiom for that is:

```asm
csrrw t0, mscratch, t0    # t0 = record pointer, mscratch = interrupted t0
```

This is correct in every reference. It relies on the handler swapping back
before `mret`.

**This handler never returns — it resets.** So it never swapped back, and
`mscratch` was left holding the interrupted `t0`. A *nested* trap then swapped
that garbage into `t0` and wrote its marker to an arbitrary address.

Where that defect lived matters more than the defect:

- Every nominal boot passed. Banner correct, timer correct, footprint correct.
- The single-fault test passed.
- The single-fault-with-corrupted-`sp` test passed.
- The defect existed **only** on the path taken when the handler itself fails.

No quantity of nominal testing reaches that path, because reaching it requires
two failures in a row. Code review would not have found it either: the idiom is
textbook-correct, and wrong here for a reason that depends on a property of
*this* handler — that it resets rather than returns.

It was found because a test existed whose entire job was to make the system fail
while it was already failing.

That is the argument this project is built on, and it is now backed by an
instance rather than an assertion: **the interesting failures live in the
recovery paths, and the recovery paths are exactly the ones ordinary testing
never executes.**

The fix is three instructions: restore `mscratch` immediately and park the
interrupted `t0` in the record instead.

---

## 3. The emulator was modelling the wrong processor

Deterministic execution is non-negotiable here: without it, the instruction
counters report host behaviour — the same workload varied 12 % between runs —
and a seeded fault injector lands somewhere different every time. QEMU's
`-icount` fixes that.

`-icount shift=N` makes one instruction consume 2^N nanoseconds of virtual time,
which means **N sets the emulated CPU speed**. The obvious choice, `shift=0`,
was taken without thinking about it.

| | `shift=0` | `shift=6` |
|---|---|---|
| Time per instruction | 1 ns | 64 ns |
| Emulated CPU | 1000 MHz | 15.62 MHz |
| Real FE310 being modelled | 16 MHz | 16 MHz |
| Host slowdown | ×161 | ×5 |

`shift=0` models a 1 GHz core driving a 32768 Hz timer: a ratio 64 times away
from the target hardware. The next milestone calibrates per-task execution
budgets against exactly that ratio, so every one of them would have been 64
times too generous — and the discovery would have come at the hardware port,
eighteen months later, with two milestones of results built on top.

It was caught by an unrelated question: how long a 72-hour soak would actually
take. It cost one flag to fix and would have cost a great deal not to.

There is a second-order trap in the same place, worth stating because the
obvious phrasing is wrong. At `shift=0`, `mcycle` and `minstret` read the same
number, which invites treating one as the other. At `shift=6` they differ by
exactly 64 — **but that is not an IPC**. `mcycle` under `-icount` counts
nanoseconds of virtual time, so it is a 1 GHz counter at every shift, and the
factor of 64 comes from a command-line flag rather than from a pipeline. A real
FE310's instructions-per-cycle is between 1 and 3. Deriving a cycle budget from
that ratio would produce a number wrong by more than an order of magnitude that
looks measured.

---

## 4. An acceptance criterion that was not merely weak but dominated

The milestone originally required: *the tick counter advances and is monotonic
across 10 million ticks.*

At 32768 Hz, 10 million ticks is 305 seconds of guest time. A low-word carry
arrives every 2^32 ticks — every 36 hours. **The criterion could never cross a
single carry**, not once, no matter how it was run.

The distinction that matters: a hollow criterion adds nothing. A *dominated* one
is strictly weaker than a test already present while implying coverage the
project does not have. It was the second.

Worse, monotonicity was the wrong property to assert. There are two naive
orderings of the two-word read, and they fail in opposite directions:

| Ordering | Wrong by | Appears to | Caught by monotonicity |
|---|---|---|---|
| high then low | ~2^32 too small | jump backwards | yes |
| low then high | ~2^32 too large | keep increasing | **no** |

The second is strictly increasing while being off by thirty-six hours. A
monotonicity assertion passes on it happily. The replacement asserts **bounded
progression** — successive readings may not differ by more than a plausible
maximum derived from the measured timebase — and was validated by building the
dangerous variant on purpose and confirming it is caught, with a reported jump
of exactly `0x1_00000000`.

The criterion was replaced by two properties that were being conflated:

- **Repeated carry propagation**: ten crossings over high-word values reaching
  byte, half-word and sign boundaries, each forced through the debugger.
- **Read stability**: 10^6 reads under the bounded-progression check, reported
  as *10^6 reads covering 81788 ticks* and never rounded up into a duration.

One detail from building the first: chaining crossings inside a single monitored
sequence turns out to be impossible, because returning the counter below the
boundary between crossings is a forward jump of 4.29e9 ticks that the
bounded-progression check correctly rejects. **The detector is strong enough to
constrain the shape of its own test.** The alternative would have been to weaken
the check to accommodate the test, which is the wrong direction.

---

## 5. The footprint moved on its own

Two commits with byte-identical source produced images differing by four bytes.
The second was a documentation-only change.

Cause: inside the linker's mergeable string pool, strings that are suffixes of
longer ones get tail-merged, and whether a given merge succeeds depends on the
*content* of the strings present. The build hash is one of those strings. So the
image size depended on the commit hash — which changes every commit, for
reasons unrelated to the code.

Four bytes of flash against a 4 MiB budget is nothing, and no optimisation was
called for. It mattered for a different reason: the project maintains a written
RAM and flash budget, and a budget is a register of claims. A claim is worth
nothing if the number can move without anyone noticing.

The fix removes the hash from the mergeable pool and pads it to a fixed width,
so image size depends on neither the content nor the length of the hash. The
durable part is the mechanism built around it: per-section sizes are pinned in a
versioned reference and compared on every measurement, and any drift fails the
build. Changing a size stays allowed; changing it silently does not.

That guard then found a real defect on its first run — the build target did not
depend on the Makefile, so editing compiler flags left a stale binary behind and
the first reference was captured from an image that no longer matched the
source.

---

## What is not proven

Stated because an unstated limit reads as a covered one.

- **No drift measurement.** The timer is shown to be self-consistent, not
  correct. Nothing here compares it to an external reference.
- **No real-time verification of the timebase.** Under `-icount` there is no
  real time inside the guest. The 32768 Hz figure rests on a host-timed
  measurement taken *without* `-icount`; the in-flight assertion checks the
  deterministic `mtime`-to-`minstret` ratio instead, which detects a changed
  machine or a changed shift but cannot separate them.
- **No 64-bit counter wraparound test.** It arrives in about 17.8 million years.
  It will never be tested. That is a decision, recorded as one.
- **No hardware.** Every figure is emulated. QEMU models no radiation, no power
  rails and no thermal behaviour, so these results are a lower bound on real
  fault rates and never an upper one.
- **The reset cause lives in RAM.** On a real FE310 it belongs in the always-on
  backup registers, which QEMU does not model. Carrying the RAM choice onto
  hardware unexamined would weaken exactly the property the reset cause exists
  to establish.
- **Emulation is more forgiving than silicon in at least one known place.** QEMU
  zeroes RAM at cold boot; real hardware powers up with noise. A persisted
  structure without a magic value and a checksum would look correct here and
  fail on a board. The test suite boots on deliberately poisoned RAM for that
  reason.

## Current figures

| | |
|---|---|
| Flash | 2978 B of a 4 MiB budget |
| RAM | 1064 B of **16384 B** — the whole of the board's memory |
| Stack peak | 96 B, measured at run time from a paint pattern |
| Fault record | 36 B in a no-init region that survives a warm reset |

## Reproducing this

```bash
sudo apt install gcc-riscv64-unknown-elf qemu-system-riscv \
                 device-tree-compiler gdb-multiarch make python3-venv
git clone https://github.com/theovil1/obc-zero.git
cd obc-zero
make measure           # refuses to run on a dirty tree, by design
make test-carry        # ten forced carries
make test-carry-broken # asserts the naive reader IS caught
make test-trap         # four fault scenarios
make test-record       # magic and checksum rejected independently
make test-poisoned     # boots on deliberately dirtied RAM
```

Every measurement in this report names a commit that can be checked out. That is
enforced rather than intended: `make measure` refuses to run on a dirty tree,
because a figure taken on uncommitted work names a state nobody else can reach.

## What comes next

The scheduler, and it is the first component whose failure will not be obvious.
A trap fault is visible, a carry defect is measurable, but a scheduler can be
subtly wrong and run correctly for months. The property to assert is not that
tasks run: it is that the order and number of executions within a window are
exactly what the task table dictates, and that they are identical between two
runs. Under `-icount` that last part can be demanded as strict equality of an
execution trace, which is a far stronger statement than a jitter bound.
