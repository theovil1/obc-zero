# Engineering logbook

Newest entry at the top. One entry per working session. Record what was
measured, not what was intended.

---

## 2026-08-20 — M2: the executive, and the property that had to come first

**Measured commit:** `4df8c5d43f306053e4b0bd8e3ee240913e1ae052`
**Toolchain:** GCC 14.2.0, QEMU 10.2.1
**Branch:** `feat/m2-scheduler` — first milestone worked on a branch.

### The assertion compares against the table, never against a tolerance

Everything before this milestone failed loudly. The scheduler is the first
component that can be subtly wrong and run correctly for months, so the property
was fixed in ADR 0003 before any code: not *that the tasks run*, but that the
order and the count in a window are exactly what the table dictates and do not
vary between runs.

Three results, each shown to fail before being trusted.

**A dropped dispatch is rejected.** `harness/broken/sched_skip.c` omits one
dispatch, once, in the middle of the window. The system boots, every task runs,
the frames are waited out, the slack is healthy, and the serial log looks
correct. Only the numbers betray it:

```
task 'housekeeping' ran 15 times, the table dictates exactly 16 (16 frames / period 1)
trace has 29 dispatches, the table dictates 30
```

That defect is what a scheduler wrong in an ordinary way looks like. Nothing in
the serial output would ever have shown it.

**An overrun is counted, and the task is not prevented.** A table whose budget
sits at 100 instructions against a task costing 2008:

```
note: task 'audit' overran 2 times (worst 2008 of 100 instructions)
16 frames, 30 dispatches, 16384 ticks spanned, order and counts match the table
```

Both halves are asserted. A test checking only the first would pass just as
happily on an implementation that killed the overrunning task — a different
system substituted for the one ADR 0003 chose, with nobody saying so.

### An assertion that was missing, found by an unexplained number

The full run took 0.2 s of host time. Sixteen frames of 1024 ticks is half a
guest second, which at the ×5 slowdown measured earlier should have been about
2.5 s. The discrepancy was the interesting part.

It turned out the earlier figure was measured with an MMIO-heavy loop and does
not generalise; the frames were being waited out correctly. But chasing it
exposed a real gap: **nothing asserted that the window took the time it should
have.** An executive could dispatch the right tasks in the right order, record
healthy slack, and never actually wait a frame — correct in everything the trace
can see, and wrong about time.

The window's start and end tick values are now recorded and the span asserted
exactly:

```
window_start 534   window_end 16918   span 16384 = 16 x 1024
```

The lesson is not the number. It is that an unexplained measurement was worth
stopping for, and that the assertion it produced is one no amount of reasoning
about the design would have suggested.

### Reproducibility: the criterion assumed something that is not true here

The criterion said to validate trace equality "by first confirming that a
deliberately non-deterministic task makes them differ". **That task cannot be
written.** Under `-icount` the guest has no source of non-determinism, which is
the whole reason `-icount` was adopted.

Removing `-icount` does make the counts differ — but the timebase assertion
refuses the boot before the window ever runs, so the sentinel never appears.
That is the correct chain and it is not a demonstration.

So the comparator is validated against two *different* images instead, and the
criterion now says so. Its standing value is as a detector of anything that
later breaks determinism, not as a check that could fail today.

### Two implementation findings

**The dump attaches to a parked target rather than breaking on a function.**
The first version broke on `print_sched_summary`, which is static and which
`-Os` inlined out of existence. The breakpoint resolved to an address the flow
never reached as GDB expected, and `continue` simply never returned. Since the
firmware parks after the window closes, no breakpoint is needed at all: attach,
read RAM, detach. That removes a whole class of fragility.

**The checker derives its expectations from the binary.** The task table is
dumped alongside the results and the expected sequence is computed from it. A
duplicated set of periods on the host would drift, and when it did the assertion
would start checking the host's opinion rather than the flight software's table.

### Measurement

`make measure` on `4df8c5d`:

```
   text    data     bss     dec     hex filename
   4134       0    1448    5582    15ce build/obc.elf

size: matches docs/size-reference.txt
deps: no compiler or library helpers linked in
```

RAM 1448 B of 16384 B. The scheduler took **384 B of its 512 B line**, 256 B of
which is the trace buffer. The task table costs nothing in RAM: it is `const`
and lives in flash, which also means a corrupted RAM word cannot change a period
or redirect a function pointer.

Eleven targets green: `test`, `test-poisoned`, `test-carry`,
`test-carry-broken`, `test-stability`, `test-trap`, `test-record`, `deps-check`,
`test-sched`, `test-sched-repro`, plus the two that must reject —
`test-sched-broken` and `test-sched-overrun`.

### Open

`ruff` is not installed, so `harness/` cannot be shown clean as the host code
rules require. Annotations and line length were checked by hand, which is a
manual verification and not the property demanded. It needs installing before
the harness grows further.

---

## 2026-08-20 — M1 closed: deliberate reset, and a guard for what the compiler adds

**Measured commit:** `76f7ca8c03fd7536db6145d7a6aa1fbd59e1e317`
**Toolchain:** GCC 14.2.0, QEMU 10.2.1

### A deliberate reset must not look like a fault

`obc_fault_reset_with_cause` records `OBC_RESET_REQUESTED` and resets through
the same AON watchdog, with the same payload-checksum-magic order. Fourth trap
mode added, and the assertion is two-sided: the line must say "requested" **and**
must not contain "trap". A one-sided assertion would pass on a record that said
both.

```
mode 4  ok - requested (watchdog, nothing was wrong)
```

The function is `KEEP`'d in the link script rather than given a caller.
`--gc-sections` dropped it, because nothing in flight code calls it yet — M5
owns the policy that decides when to ask for a reset. Inventing a call site to
satisfy the linker would have misrepresented which milestone owns what, and the
comment in the link script says so.

### The rule "32-bit arithmetic everywhere" was too weak to hold

Yesterday's `__udivdi3` finding produced a discipline. A discipline is not a
property. The compiler emits calls the source never mentions: 64-bit division
and shift, `memcpy` from a large struct initialiser, `memset` from a large
zeroing. None of them appear in a grep of `flight/`.

A freestanding link already fails on them, loudly, which is correct. What it
cannot catch is **the fix**: adding `-lgcc` or `-lc` to make the error go away
pulls the helper in and the build goes green carrying a dependency nobody
decided to take.

Demonstrated rather than argued. With a 64-bit division in `obc_main`:

| Link | Result |
|---|---|
| `-nostdlib` alone | fails: `undefined reference to __udivdi3` |
| `-nostdlib -lgcc` | **succeeds**, `__udivdi3` in the symbol table |
| `make deps-check` on that image | **fails**, naming the symbol |

`deps-check` runs on every `make measure`. The undefined set must be empty and
no helper may appear anywhere in the symbol table. Same mechanism as the size
reference: a discipline converted into something checked every time.

### The timebase message names both hypotheses

The ratio is fixed jointly by the timer frequency and the `-icount` shift, so it
cannot distinguish them — and the *nominal* reason for it to move is someone
deliberately changing the shift. A message blaming the timer would send the next
person hunting a regression that does not exist:

```
base   : 3809426 milli-instr/tick, expect 476837 FAULT out of tolerance
         either -icount shift is not 6 (the usual cause, and deliberate)
         or the machine timer is not 32768 Hz (a real regression)
```

An assertion that fires correctly but explains itself wrongly costs as much time
as one that does not fire.

### The M5 counter needs its own record, not a field

Following through on the reset-loop question rather than leaving it at "the
persistent cause is not enough". If the counter must survive `consume()`, it
cannot live under the cause's magic and checksum: clearing the record clears the
commit point and everything inside goes with it. Sharing a checksum is worse —
updating the counter would invalidate the cause's, and vice versa.

So: a second record in `.noinit`, with its own magic and checksum. Two valid
structures with different lifetimes, one consumed every boot and one persisting
across many. A paragraph now against a rewrite of the record layout and every
assembly offset midway through M5.

### Measurement

`make measure` on `76f7ca8`:

```
   text    data     bss     dec     hex filename
   2978       0    1064    4042     fca build/obc.elf

size: matches docs/size-reference.txt
deps: no compiler or library helpers linked in
```

Flash 2978 B of 4 MiB, RAM 1064 B of 16384 B. Eight targets green: `test`,
`test-poisoned`, `test-carry`, `test-carry-broken`, `test-stability`,
`test-trap` (four modes), `test-record` (three modes), `deps-check`.

### M1 status

Every acceptance criterion is met except the one naming `harness`, which stays
open deliberately: the injectors moved to `harness/faults/` during this
milestone, but the run lifecycle and the assertions are still in the Makefile
until M9 consolidates them into `harness/runner/`. The tick should describe the
structure, not only the behaviour.

---

## 2026-08-20 — The thesis, demonstrated on my own code

**Measured commit:** `1d767f97600c91d834a7b47b8323485e2e75ba32`
**Toolchain:** GCC 14.2.0, QEMU 10.2.1

### Why the mscratch defect is the result worth keeping

The `README` claims that a flight computer is credible only through evidence,
and that surviving one's own faults is the product rather than a feature. That
is easy to write and easy to leave as a slogan. This milestone produced a
concrete instance of it, in this repository's own code.

The defect: the trap handler used the standard `csrrw t0, mscratch, t0` idiom,
which leaves `mscratch` holding the interrupted `t0` and assumes the handler
swaps it back before `mret`. This handler resets instead of returning, so it
never swapped back. A **nested** trap therefore swapped garbage into `t0` and
wrote its marker to an arbitrary address.

What matters is not the bug. It is where the bug lived:

- Every nominal boot passed. The banner was correct, the timer was correct.
- The single-fault test passed, twice over, including with a corrupted `sp`.
- The defect existed **only** on the path taken when the handler itself fails —
  that is, only after everything else has already gone wrong.

No quantity of nominal testing reaches that path. No amount of code review found
it either; the idiom is correct in every textbook, and wrong here for a reason
that depends on a property of *this* handler. It was found because a test
existed whose entire job was to make the system fail while it was already
failing.

That is the project's argument, and it is now backed by an instance rather than
an assertion: **the interesting failures live in the recovery paths, and the
recovery paths are exactly the ones ordinary testing never executes.** Worth a
write-up later, with the diff.

### Timebase asserted by ratio, not by the host clock

The M1 criterion asked for an assertion against a host-timed interval. Under
`-icount` that measures the wrong thing: guest time advances with instructions
executed, so a host-timed figure reports emulation speed and host load, and
moves with the machine that ran it.

The deterministic anchor is the ratio between the two clocks already present. A
continuously executing core retires exactly `(10^9 / 2^shift) / 32768`
instructions per tick — 476.837 at `shift=6`. Measured 476822 in a probe and
476162 to 476786 in the boot check, inside a 1 % tolerance.

Shown to fail before being trusted:

| Shift | Reported | Verdict |
|---|---:|---|
| 6 | 476786 | ok |
| 3 | 3807794 | FAULT, out of tolerance |
| 0 | 30473666 | FAULT, out of tolerance |

It does **not** establish 32768 Hz in real-time terms, and cannot: under
`-icount` there is no real time inside the guest to compare against. The
absolute figure still rests on the host-timed measurement taken without
`-icount`, recorded in ADR 0001. Stated in the code so that nobody later reads
this check as proving more than it does.

A constraint found on the way: 64-bit division pulls `__udivdi3` out of libgcc,
which a freestanding link does not provide. The arithmetic is 32-bit throughout.
The build fails loudly rather than silently, but it is worth knowing before
writing the expression rather than after.

### The harness moved while it was still small

`harness/faults/` now holds the four injectors and `harness/broken/` the
deliberately naive timer. Seven files today; the same move at M9 would be thirty
and would happen at the worst possible moment, if at all.

The run lifecycle and the assertions are still in the Makefile, and the
criterion naming `harness` stays **unticked** on purpose. The harness is being
built incrementally across milestones — M9 consolidates it, it does not begin
there — and the tick should reflect the structure rather than only the
behaviour.

### What the persistent cause does not give M5

Checked against `flight/core/fault.c` rather than assumed. The cleared re-entry
flag answers "am I inside the handler on this boot"; M5's reset-loop protection
needs "did previous boots die inside the handler", and the persistent
double-fault cause is **not** sufficient for it:

- there is no count, so a boot cannot tell the first double fault from the
  hundredth — which is the entire distinction loop protection is made of;
- `obc_fault_consume()` clears the record after reporting, so even the single
  bit does not survive into the boot after next;
- there is no window, and "N in a row" and "N within T ticks" are different
  policies needing a persisted timestamp nothing currently writes.

Written into the backlog with what M5 will need, because the counter has to
survive `consume()` — which makes it a change to the record's structure rather
than an addition beside it, and cheaper to plan now than to retrofit while
building the escalation ladder.

### Flash is now tracked

The image tripled on this milestone, 874 to 2698 bytes. On 4 MiB that is
nothing, and no optimisation is called for. But `BUDGET.md` tracked only RAM,
while rule 3 of that same file actively pushes constant data into flash, and
M6 through M8 each add tables and fixed-layout frames.

A flash row now exists with an alert threshold of 262144 bytes, one sixteenth of
the region, **declared arbitrary in the file itself**. It is not derived from
anything; it exists so that crossing it forces a conversation instead of passing
unnoticed.

### Measurement

`make measure` on `1d767f9`:

```
   text    data     bss     dec     hex filename
   2698       0    1064    3762     eb2 build/obc.elf
```

Flash 2698 B of 4 MiB. RAM 1064 B of 16384 B, of which 36 B is the fault record
in `.noinit`. Seven test targets green: `test`, `test-poisoned`, `test-carry`,
`test-carry-broken`, `test-stability`, `test-trap`, `test-record`.

### Still open in M1

The reset cause produced by the AON watchdog path in normal operation, as
opposed to the trap path. And the harness criterion above.

---

## 2026-08-20 — M1: the trap handler, and the defect its own test found

**Measured commit:** `9e807dfdec5c739d94a2dc2d1b6de1bec5766b32`
**Toolchain:** GCC 14.2.0, QEMU 10.2.1

### The handler never touches the stack

`mscratch` holds the fault record address; `sp` is never read or written. If the
fault was a stack overflow or a corrupted `sp`, saving a context frame would
fault again immediately, and the second trap overwrites `mepc` and `mcause` —
destroying the original cause and leaving only the handler's own fault visible.

Verified rather than argued. Fault injected with `sp = 0x40000000`, outside RAM:

```
reset  : trap mcause=0x00000001 (exception 1) epc=0x00000000 tval=0x00000000
```

The original cause, not a store fault of the handler's own making. The assertion
is on the *cause*, not on the existence of a record — a handler that produced
some record while losing the cause would pass the weaker test.

### `mscratch` and `mtvec` are set four instructions in

Before the `.data` copy and the `.bss` clear, either of which could fault on a
corrupted image. A trap before `mscratch` is loaded would write the record to
address zero; a trap before `mtvec` is installed would jump to address zero.
Both windows are closed by ordering, and the banner asserts `mscratch` at run
time rather than trusting where those instructions sit:

```
mscratch: 0x80000004 ok
```

### The double-fault test found a real defect on its first run

This is the result worth keeping from this session.

The classic idiom is `csrrw t0, mscratch, t0`, which leaves `mscratch` holding
the interrupted `t0` and expects the handler to swap back before `mret`. This
handler never returns — it resets — so `mscratch` stayed clobbered. A nested
trap then swapped that garbage into `t0` and wrote the double-fault marker to an
arbitrary address. The symptom was a second boot reporting no record at all
where a double fault was expected.

`mscratch` is now restored immediately and the interrupted `t0` parked in the
record instead. Three extra instructions.

The defect was invisible to both other trap tests, which never re-enter the
handler. It existed only in the path that runs when everything else has already
gone wrong — which is the path that matters most and the one least likely to be
exercised by accident.

### The re-entry flag: cleared, not persisted, and why

The flag lives in `.noinit` beside the cause but is **cleared by the startup
code** rather than protected by the magic-and-checksum discipline. The two do
not prove the same thing, so the choice is recorded rather than left implicit.

Cleared, the flag means "am I already inside the handler on this boot", which is
the question the handler asks at entry. Persistent, it would mean "some previous
boot died inside the handler" — also useful, but a different fact, and
conflating them would make a genuine double fault indistinguishable from a stale
flag carried across a reset.

At cold boot the word holds whatever RAM held. On silicon that is noise, and a
non-zero pattern would make the first trap of the first boot report a double
fault. QEMU zeroes RAM and would hide this entirely, which is exactly why the
poisoned-RAM run exists:

```
reset  : none recorded (cold boot or torn write)
boot   : ok
PASS (seed=1)
```

### Write order and the two rejections

Payload, then checksum, then magic. The magic is the commit point: a reset
landing mid-write leaves a record with no magic, which the reader rejects rather
than half-believes. The reverse order would open a window in which a torn record
reads back as authentic.

Both rejections tested separately, because one does not cover the other — a
validator checking only the magic passes the wrong-checksum case, and one
checking only the checksum passes the wrong-magic case:

| Planted record | Reported |
|---|---|
| magic valid, checksum wrong | `RECORD CORRUPT: magic valid, checksum wrong` |
| checksum valid, magic wrong | `none recorded (cold boot or torn write)` |
| both valid | `trap mcause=0x8000000B (interrupt 11) epc=0x20400123` |

The third line carries a second result: `0x8000000B` is reported as **interrupt
11**, not exception 11. `mcause` is stored and reported whole, so the two events
that share code 11 stay distinguishable. A truncated field would make them one
line in a log, and that ambiguity would cost a night during the first real
anomaly.

The checksum is a XOR of four words with a seed — a garbage detector, not an
error-correcting code. It accepts random RAM with probability 2^-32 and offers
nothing against a correlated corruption that preserves the XOR. Stated rather
than papered over.

### Three trap scenarios, in increasing order of what they rule out

| Mode | Injection | Assertion |
|---|---|---|
| 1 | `pc = 0`, valid `sp` | cause recorded, exactly two banners |
| 2 | `pc = 0`, `sp` outside RAM | the **original** cause, not the handler's |
| 3 | `pc = 0` from inside the handler | double fault, distinct cause, one reset |

Every mode asserts exactly two banners. Three or more would be a reset loop, and
a loop that eventually settles would otherwise read as a pass.

Faults are produced by moving the program counter to an unmapped address rather
than by planting a trap instruction: flash is read-only here, and a build
carrying a deliberate fault would not be the binary under test.

### Measurement

`make measure` on `9e807df`:

```
   text    data     bss     dec     hex filename
   2430       0    1064    3494     da6 build/obc.elf
```

Flash 2430 B, up 984 B for the handler, the record and the reporting. RAM
1064 B of 16384 B, up 36 B for the fault record in `.noinit`. Still 6.5 percent
of RAM; `docs/BUDGET.md` is unaffected.

### Still open in M1

The timebase assertion at 32768 Hz, and the harness-side assertion of the reset
cause. The latter's assertions exist and run, but they live in the Makefile
rather than in `harness/`, which does not exist until M9 — left unticked because
the criterion names the harness.

---

## 2026-08-20 — shift=6, and the two properties the old criterion conflated

**Measured commit:** `a8acc02b4bf127dd87a3739528bf07cd520346a8`
**Toolchain:** GCC 14.2.0, QEMU 10.2.1

### The old criterion was dominated, not merely hollow

"Monotonic across 10 million ticks" is 305 guest seconds at 32768 Hz. A carry
arrives every 2^32 ticks, once every 36 hours. So it could never cross a single
carry — it was strictly weaker than the carry test written beside it, not just
weak in isolation. Worth the distinction: a hollow criterion adds nothing, a
dominated one also implies coverage it does not have.

It was aiming at two different properties without naming either. Named and
tested separately now.

### `shift=0` was the wrong emulated CPU

Chasing the soak arithmetic turned up something better. `shift=N` sets virtual
time per instruction, so it sets the emulated CPU speed:

| | `shift=0` | `shift=6` |
|---|---|---|
| Time per instruction | 1 ns | 64 ns |
| Emulated CPU | 1000 MHz | **15.62 MHz** |
| Real FE310 | 16 MHz | 16 MHz |
| Host slowdown | ×161 | **×5** |
| Determinism | yes (`dinstret=6006`) | yes (`dinstret=6006`) |

`shift=0` modelled a 1 GHz core driving a 32768 Hz timer: 64x away from the
target. Every M2 budget calibrated against it would have been 64x too generous
relative to the clock, and would have needed recalibrating at the hardware port.
It was also 32x slower for nothing. Caught before M2 rather than during it.

A useful side effect: at `shift=0`, `mcycle` and `minstret` were both 6006,
which invites reading one for the other. At `shift=6` they read 384384 and 6006,
differing by exactly the shift. `minstret` is now visibly the instruction count.

### Repeated carry propagation

Ten crossings, high-word values 0, 1, 2, 255, 256, 65535, 65536, 2147483647,
2147483648, 4294967294 — reaching byte, half-word and sign boundaries that ten
consecutive values would never touch. All survived.

One crossing per run, and the reason is a finding rather than a workaround.
Chaining crossings inside one monitored sequence is impossible: after a crossing
the counter sits just above the boundary, and bringing it back below for the
next one is a forward jump of ~4.29e9 ticks, which the bounded-progression check
correctly rejects as implausible. **The detector is strong enough to constrain
how its own test can be built.** Attempting it anyway would have meant weakening
the check to accommodate the test, which is the wrong direction.

### Read stability

```
tick   : ok, max delta 0x00000000:0x00000001 ticks
reads  : 1000000 covering 81788 ticks
```

A distinct property: a reader that slipped occasionally would survive any number
of forced carries. Stated as reads and ticks, never rounded up into a duration —
under `-icount` the relation between instructions, guest ticks and wall clock is
a build parameter, so "stable for X seconds" is a claim this test cannot make.

### The corollary that lands on M10

Running everything under `-icount` means a long-duration claim is bounded by
instructions, not elapsed time. Measured at `shift=6`, slowdown ×5:

| Claim | Cost |
|---|---|
| 72 hours of **guest** time | ~360 host hours, 15 days |
| 72 hours of **host** time | ~14.4 guest hours |

At `shift=0` those rows read 14241 host hours and 21.8 guest minutes. Decided in
ADR 0002 rather than left for the report: M10's soak is **guest** time, since a
soak measures how long the flight software ran, not how long a workstation was
busy. Running 72 host hours and calling it a 72-hour soak would overstate
coverage fivefold and be indistinguishable in the report from the honest
version.

### What M1 does not prove, written down

Drift against a reference clock — the counter is shown self-consistent, not
correct. Timing behaviour on silicon — every figure is emulated and the
CPU-to-timer ratio is a chosen parameter. 64-bit wraparound — ~17.8 million
years away, will never be tested, and that is a decision rather than an
oversight, which is why it is recorded as one.

### Injection proof is now a general rule

The three false-pass defects found yesterday are written into M9's scope rather
than left as a local fix: every injector emits a positive assertion that the
fault landed, and the run fails without it. A return code says the tool exited,
not that anything was injected. A test reporting PASS on a fault that never
happened is worse than no test, because it is counted as coverage.

### Measurement

`make measure` on `a8acc02`:

```
   text    data     bss     dec     hex filename
   1446       0    1028    2474     9aa build/obc.elf
```

Flash 1446 B, up 92 B for the span reporting. RAM unchanged at 1028 B of 16384 B.

---

## 2026-08-20 — M1, first part: the machine timer and a forced carry

**Measured commit:** `0d47f56d5001a81473e2086862f489b254a4e626`
**Toolchain:** GCC 14.2.0, QEMU 10.2.1

### The race is forced, not awaited

`mtime` is 64 bits behind two 32-bit registers, so a read can straddle the
low-word carry. Waiting for that carry is not a test strategy: it happens once
every 36 hours at 32768 Hz, and under `-icount` the run is deterministic, so a
given run either meets the window or never will. Never would leave a green test
on a broken clock forever.

`harness/faults/carry.gdb` parks the counter below the boundary and pushes it across while
the reader sits between its two loads. The test no longer depends on the
timebase at all, and it is the first time the injection primitives are used
against a genuine defect rather than an intentionally broken build.

### Bounded progression, because monotonicity passes on a broken clock

There are two naive orderings and they fail in opposite directions:

| Ordering | Wrong by | Appears to | Monotonicity catches it |
|---|---|---|---|
| high then low | ~2^32 too small | jump backwards | yes |
| low then high | ~2^32 too large | keep increasing | **no** |

`harness/broken/mtime_naive.c` implements the second on purpose, and is caught:

```
tick   : FAULT implausible jump of 0x00000001:0x00000000 ticks
boot   : FAULT
PASS (the broken build was correctly rejected)
```

Exactly 2^32, and strictly increasing. A monotonicity assertion would have
waved it through. The correct reader under the same injection reports a maximum
delta of `0x105`, 261 ticks, which is the predicted value.

### Bounded retry, on the loop that looks harmless

The high / low / re-read-high sequence loops while the high word moved. Two
iterations are provably enough. That is an argument, not a bound, and this is
exactly the loop where the rule gets forgotten because it looks innocent. The
budget is capped at four; exhaustion returns `OBC_ERR_UNSTABLE` and is counted,
turning an impossible timer into a status the caller must handle.

### Two defects in the test harness, both producing false passes

Found while building it, and worth recording because they are the same failure
mode the milestone is about.

**The debugger script errored and the test still reported PASS.** `break
$carry_line` is rejected by GDB, which requires an integer for a line-spec
convenience variable. The script aborted, the firmware ran undisturbed, the
sentinel appeared, and the run was green on an injection that never happened.
The script now prints `CARRY-INJECTED` after writing guest state and the run
fails if that proof is absent.

**The injection landed on the wrong read.** It hit the baseline reading that the
loop compares against, so the baseline moved with everything else and the
observed delta was zero — a pass with nothing exercised. The baseline read is
now allowed through and the injection targets the one after it.

A third: `MTIME_SRC` was overridden only for the sub-make that builds, so the
broken run compiled the naive reader but pointed the debugger at the flight
source. The breakpoint resolved against a file absent from the binary and the
run hung rather than failing. The whole run now happens inside the sub-make, and
the debugger has a hard timeout so a missing breakpoint can never hang again.

### Measurement

`make measure` on `0d47f56`:

```
   text    data     bss     dec     hex filename
   1354       0    1028    2382     94e build/obc.elf
```

Flash 1354 B, up 480 B from 874 for the timer and the self-check. RAM 1028 B of
16384 B, up 4 B for the unstable-read counter. Stack peak 96 B, up from 64 B.
All within the M0 stack allocation of 1024 B; `docs/BUDGET.md` is unaffected.

### One criterion deliberately left unticked

"Tick counter advances and is monotonic across 10 million ticks" is **not**
done. The boot self-check takes 64 readings, which under `-icount` all land
inside a single 30.5 us tick, so the maximum delta it observes is zero. It shows
that reads succeed and never regress; it shows nothing about ten million ticks.

Ticking it would have been the same hollow claim this milestone exists to
remove, so it stays open with the reason written next to it. Either implement
the long span honestly, or replace the criterion with what the carry test
already proves.

---

## 2026-08-20 — `-icount`, and why 32768 Hz forces a second time domain

No commit measured: this records properties of the emulator, decided before M1
so that M2 and M9 are not built on the wrong answer. Conclusions in
`docs/adr/0002-time-domains.md`.

### The consequence of 32768 Hz that I had not drawn

ADR 0001 established the timebase and I treated it as a timekeeping fact. It is
also a **resolution** fact, and that lands on M2.

One tick is 30.5 µs. A task running in a few microseconds measures zero or one
tick. So `mtime` cannot express a per-task execution budget at all, and a budget
built on it would report zeros and pass its tests by accident — the exact
failure mode this project exists to catch, arriving through the front door.

### `mcycle` and `minstret` are unusable without `-icount`

Same 1000-iteration loop, four runs:

| Run | `mcycle` Δ | `minstret` Δ |
|---|---:|---:|
| 1 | 97664 | 113434 |
| 2 | 92658 | 91884 |
| 3 | 102682 | 100708 |
| 4 | 93378 | 92170 |

Twelve percent of spread. `minstret` is no better than `mcycle`, which is the
tell: QEMU is reporting the host, not the guest.

With `-icount shift=0`, the same workload gives `mcycle = minstret = 6006` on
every run. Their equality is worth noticing rather than enjoying: under
`-icount`, QEMU advances one cycle per instruction, so **`mcycle` is an
instruction count wearing a cycle count's name.** It says nothing about pipeline
or cache behaviour and must never be published as a duration.

### Execution position becomes reproducible, which closes the M9 question

Stepping a fixed number of instructions from a breakpoint, three runs each:

| | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| Without | `minstret=644399082` | `minstret=-744588599` | `minstret=2138342361` |
| With `-icount` | `minstret=1096` | `minstret=1096` | `minstret=1096` |

ADR 0001 recorded that the gdbstub attaches in wall-clock time, so a seeded
injector would land somewhere different every run, and left the fix to be chosen
at the start of M9 between an instruction-counter breakpoint and a TCG plugin.
`-icount` settles it with a build flag: an injection point is a `minstret`
value. No plugin to write.

Deciding this now rather than at M9 was the whole point. Adopting it after M2
had calibrated budgets and M9 had built injectors would have invalidated both.

### A warning that did not hold, checked rather than accepted

`mcycle` and `minstret` were expected to be silently read-only under QEMU. They
are not, on 10.2.1. Writing `0x40000000` to `mcycle` and reading it back one
instruction later:

```
mcycle   ecrit=0x40000000  relu=0x40000001  ecart=1
minstret ecrit=0x50000000  relu=0x50000001  ecart=1
```

The delta of 1 is the intervening `csrr`. The write lands exactly. Without
`-icount` it lands too, but the read-back has drifted by ten thousand host
cycles — another view of the same nondeterminism.

So the preset-and-provoke technique used for the `mtime` carry test applies to
these counters as well, and acceptance criteria may rely on it. Worth stating
because the opposite was the working assumption ten minutes earlier.

### Applied

`-icount shift=0` is now on every QEMU invocation in the Makefile, not only on
campaigns: a test running under different execution semantics from the campaign
is not testing the campaign. No measurable cost — the smoke test runs in 0.08 s.

Two time domains, with non-overlapping authority: `mtime` for scheduling,
deadlines and anything published; `minstret` for budget enforcement and
injection coordinates, never for a duration. M2 budgets will be expressed in
retired instructions rather than milliseconds, which is a change to how that
milestone was described.

### Also

`docs/size-reference.txt` now carries the toolchain versions in its header, so a
compiler upgrade produces a diff whose first line explains the drift instead of
an unexplained failure.

The unmodelled AON backup registers are recorded in ADR 0001 as a **porting
obligation** rather than an emulator quirk. On a real FE310 the reset cause
belongs there, outside the RAM a brownout can disturb; keeping it in RAM is a
consequence of the model and must be revisited on hardware, not inherited.

### Poisoned RAM, so M1 cannot be flattered by the emulator

`make test-poisoned` fills the whole of RAM with a seeded pattern through the
gdbstub before releasing the CPU, then boots normally. The image under test
therefore starts on dirty memory rather than on QEMU's complimentary zeros.

Verified rather than assumed, because a poison that silently failed to land
would leave a test that passes for the wrong reason. Expected word at offset
`0x3000` from the generated file, then read back from the guest after boot:

```
expected 0x80003000 = 0x0919C2B6
0x80003000:	0x0919c2b6	0x60105a30
0x80000000:	0xdeadbeef	0xdeadbeef
```

The poison survives where nothing writes over it, and `start.S` has repainted
the stack on top of it at `0x80000000`. That untouched region is exactly where
M1's reset-cause structure will live, so the magic value and the checksum will
be tested against noise from the first run rather than against zeros that make
them look unnecessary.

Deterministic given `POISON_SEED`, which is reported on every run so a failure
reproduces from the seed alone.

### Correction

I attributed the 32768 Hz figure to a targeting discussion. It came from
measurement here, against wall clock. The figure that came from a targeting
assumption was 10 MHz, which belongs to the `virt` machine and appeared in an
early draft of a board header that was later deleted. The distinction matters
only because a measured value and an assumed one deserve different amounts of
trust.

---

## 2026-08-20 — History rewritten to correct the author identity

No measurement here. Recorded because every commit hash in this file changed,
and a reader finding shifted hashes with no explanation would be right to
distrust the rest.

All nine commits were authored with a private address. GitHub refuses a push
carrying one, and the refusal arrives at push time rather than at commit time,
so the problem surfaces long after it is created. Author and committer were
rewritten to `288923546+theovil1@users.noreply.github.com`.

**Content is untouched.** Every rewritten commit has a byte-identical tree to
its original; the check was run pair by pair. Only author, committer and the
hashes that follow from them changed. Measurements recorded against the old
hashes remain valid, because they measured the build produced by that tree and
the tree is the same.

Three commit *messages* cited hashes of earlier commits, and rewriting would
have left them pointing at commits that no longer exist. The rewrite was
therefore redone as a single pass over the original history, translating those
citations as it went, rather than as a rewrite followed by a patch-up. The
mapping:

| Before | After |
|---|---|
| `d668b4d` | `a196c30` |
| `83fbe8b` | `ffbf6e0` |
| `0205847` | `dc918ca` |
| `58199cd` | `c79bf94` |
| `578246b` | `3ca4ce4` |
| `1d9ae85` | `85015a0` |
| `976391f` | `4bb1da9` |
| `65b0d67` | `9031860` |
| `673b427` | `c0c746e` |

References in this file and in ADR 0001 were remapped with the same table. One
consequence worth stating plainly: the build-hash experiment in the entry below
was run with the *pre-rewrite* strings, so the table there now shows the new
hashes rather than the literal values passed on the command line. The lengths
are the same, seven and thirteen characters, so the result it demonstrates is
unaffected — but the strings shown are not the ones typed.

Nothing had been pushed, so no published history was rewritten. Doing this after
a push would have been a different matter entirely, and is the reason it was
worth doing now rather than later.

---

## 2026-08-20 — Size guard, and the M1 platform facts

**Measured commit:** `9031860f2ea1bfbc486934c614f008e0dd730a29`
**Toolchain:** GCC 14.2.0, QEMU 10.2.1

### The guard rail earned its place immediately

Per-section sizes are now pinned in `docs/size-reference.txt` and compared on
every `make measure`. Drift fails the build. Validated by breaking it first: a
one-character edit to a banner string moved `.rodata` from 204 to 208 and was
caught, then passed again on restore.

Then it found something real, which is the whole argument for having it.

`$(TARGET)` did not depend on the `Makefile`. Editing build flags therefore left
a stale ELF in place, and the very first size reference was captured from a
binary that no longer matched the source. The reference read 188 while the true
figure was 196.

Chasing that 8-byte gap exposed an **incomplete fix from earlier the same day**.
Moving the build hash out of the mergeable string pool removed the dependency of
image size on hash *content*. It did not remove the dependency on hash *length*:
`4bb1da9` is 7 characters and `4bb1da9-dirty` is 13, and the padded difference
is exactly 8 bytes. A `-dirty` build was larger than a clean one, so `size-check`
would have failed spuriously on any uncommitted work.

The hash is now padded to a fixed 20-character field. Held under experiment:

| Build hash | `.rodata` | `.text` |
|---|---:|---:|
| `dc918ca` | 204 | 532 |
| `4bb1da9-dirty` | 204 | 532 |
| `a` | 204 | 532 |
| `v1.2.3-rc1-44-gdeadbee-dirty` | 204 | 532 |

Identical across one character, thirteen, and one longer than the field itself.
The banner now carries trailing spaces; that is the visible cost of a size that
does not move.

Proof the fix holds end to end: the reference was committed from a *dirty* build
and matched exactly on the *clean* one at `9031860`. Under the old behaviour it
could not have.

The lesson is not about four bytes. I declared this fixed once already, and it
was only half fixed. What caught the remainder was a mechanism that checks every
time, not a second look.

### Debugger: the write half, which is what M9 actually needs

Reading a register proved the chain was alive. Fault injection needs to *write*.
Verified against a running target, stopped at a breakpoint on `obc_main`:

```
--- read stack paint:
0x80000000:	0xdeadbeef	0xdeadbeef
--- write:
0x80000000:	0x12345678	0xdeadbeef
--- single bit flip:
0x80000000:	0x12345678	0xdeadbeee
--- register write:
$1 = 0xcafe
```

Breakpoint on symbol, RAM read, RAM word write, single-bit flip, register write.
Every primitive `harness/faults/` is specified to need. The remaining M9 question
is unchanged and is about *when* an injection lands, not whether it can.

### M1 platform facts, established before writing M1

All four checked against the machine, in the same spirit as the reset vector.

**`mtime` is at `0x0200BFF8`, `mtimecmp` at `0x02004000`, both writable.** ADR
0001 previously flagged these as assumed from the standard SiFive layout. They
are now verified, and the writability is what makes the carry test possible.

**The timebase is 32768 Hz, not 10 MHz.** Measured against a host-timed
interval: 32768 ticks elapse in 1.04 s wall clock including QEMU start-up. This
is the FE310 low-frequency clock. The 10 MHz figure belongs to the `virt`
machine, and I had carried it in my head from the earlier targeting discussion.
Using it would have made every deadline wrong by a factor of 305, silently, with
nothing failing. The acceptance criteria now assert the timebase so that a change
of machine cannot rescale the system unnoticed.

**The AON watchdog produces a real reset.** `wdogcfg.rsten` set, and the boot
banner repeats. So "clean reboot" at M1 can mean an actual reset rather than a
jump to `0x20400000`, which would re-run startup while leaving peripherals
configured — a different event that must never be described as the same one.

**The AON backup registers are not modelled.** Writing `0xA5A5A5A5` to
`0x10000080` reads back `0x00000000`. The write is discarded. The place real
hardware keeps a reset cause does not exist here.

**RAM survives a warm reset; QEMU zeroes it at cold boot.** A sentinel written
before a watchdog reset is still there afterwards:

```
BOOT noinit=0x00000000
BOOT noinit=0xC0FFEE01  <== SURVIVED THE RESET
```

So persistence works, and the reset cause lives in RAM excluded from `.bss`
zeroing. But the first line is the trap: QEMU gives a clean zero at cold boot
where silicon gives garbage. An unprotected read looks correct here and fails on
hardware. Magic *and* checksum are mandatory, and no test in emulation can
justify dropping either — this is a case where the emulator is more forgiving
than reality, which is the direction that hides defects rather than inventing
them.

### Acceptance criteria corrected before any test was written

The M1 criterion "monotonic across 10 million ticks" could not catch the defect
it appeared to guard. `mtime` is 64 bits read through two 32-bit accesses, and a
naive read goes backwards on the low-word carry — which happens every 2^32
ticks, once every 36 hours at 32768 Hz. Ten million ticks never reaches one.

The criterion now provokes the carry by presetting `mtime` below the boundary,
and the test must first be shown to fail against a deliberately naive read.
Criteria added for the timebase and for rejecting a reset cause whose magic or
checksum is wrong.

### Measurement

`make measure` on `9031860`:

```
   text    data     bss     dec     hex filename
    874       0    1024    1898     76a build/obc.elf

size: matches docs/size-reference.txt
```

Flash 874 B, up 16 B from 858 for the fixed-width hash field. RAM unchanged at
1024 B of 16384 B. Stack peak unchanged at 64 B.

---

## 2026-08-20 — Debugger chain verified end to end

No commit is named here: this records a property of the development host, not a
measurement of a built image, so the clean-tree measurement rule does not apply.

### `gdb-multiarch` does carry RISC-V

Its banner is actively misleading:

```
This GDB was configured as "x86_64-linux-gnu".
...
The target architecture is set to "riscv:rv32".
```

The first line names its *host*; the second shows the target accepted anyway.
Reading the first line as a statement about targets is what sent me toward
building a debugger from source, which was never necessary. There is no
`riscv64-unknown-elf-gdb` in any apt package, and none is needed.

### Attached and cross-validated the reset vector

```
0x00001004 in ?? ()
pc             0x1004	0x1004
=> 0x1004:	lui	t0,0x20400
   0x1008:	jr	t0
```

Worth more than a connectivity check: this reproduces, through an independent
tool, the reset vector ADR 0001 derives from the QEMU monitor. Two tools, same
answer, so the foundation the link script rests on is not resting on one
reading.

### Two traps in the connection

**Order matters.** `set architecture riscv:rv32` must precede `target remote`.
On an already-attached target GDB auto-detects and falls back to its host
architecture.

**A missing QEMU reports a timeout, not a refusal.** Held under experiment:
`:1234` and `localhost:1234` both succeed when QEMU is listening, and both
produce the identical timeout message when it is not. The syntax is a red
herring; the message simply does not say what it means. Recorded because it will
be met again at M9, when the harness attaches to the stub thousands of times and
will need to tell "not listening yet" from "listening but wedged".

### Tooling corrections applied

- `make gdb` documented as the half that starts QEMU; `make attach` added for
  the half that connects.
- `GDB` in the Makefile pointed at `gdb-multiarch` instead of a binary that
  does not exist.
- Toolchain list corrected: the RISC-V QEMU binaries are in `qemu-system-riscv`,
  not `qemu-system-misc`, on Ubuntu 26.04.

---

## 2026-08-20 — Footprint was not a stable metric

**Measured commit:** `3ca4ce4ce008c34e0894c46817d11844879a1b64`
**Toolchain:** GCC 14.2.0, QEMU 10.2.1

Found while checking an unexplained 4-byte discrepancy between two builds. Worth
the detour: the metric `docs/BUDGET.md` is built on was moving on its own.

### What happened

`c79bf94` measured 830 B of text; its parent `dc918ca` measured 826 B. `c79bf94`
is a documentation-only commit — the source is byte-identical. A footprint that
moves when no code changed is not a footprint.

My first explanation was that the hashes were different lengths. They are both
seven characters. Wrong, and worth stating plainly because it was written into
this logbook before it was checked.

### The actual cause

Dumping `.rodata` for both builds showed the difference: the standalone `"\r\n"`
literal exists as its own entry in one image and not in the other. Inside the
mergeable string pool the linker tail-merges strings that are suffixes of longer
ones, and whether that merge lands depends on the *content* of the strings
present, not their length.

Held under experiment, varying only the hash:

| Build hash | `.text` | `.rodata` |
|---|---:|---:|
| `dc918ca` | 826 | 184 |
| `c79bf94` | 830 | 188 |
| `aaaaaaa` | 830 | 188 |
| `bbbbbbb` | 830 | 188 |
| `zzzzzzz` | 830 | 188 |

`dc918ca` is the outlier: it got a merge the others did not. The 826 B recorded
as the M0 reference was a lucky hash, not a smaller program.

### Fix

The hash now lives in its own section instead of being concatenated into a
string literal, so it never enters the mergeable pool. Verified across six
different hashes, all producing an identical image size:

```
hash=dc918ca  text=858  rodata=188
hash=c79bf94  text=858  rodata=188
hash=aaaaaaa  text=858  rodata=188
hash=bbbbbbb  text=858  rodata=188
hash=zzzzzzz  text=858  rodata=188
hash=deadbee  text=858  rodata=188
```

Costs 28 B of flash against a 4 MiB budget, and buys a number that means
something when compared across commits.

### Measurement

`make measure` on `3ca4ce4`:

```
   text    data     bss     dec     hex filename
    858       0    1024    1882     75a build/obc.elf
```

RAM is unchanged at 1024 B of 16384 B; this was never a RAM issue. Flash is
858 B, and that figure is now stable under a change of hash.

### What this says about the rest

Flash is not scarce here, so nobody would have chased 4 bytes for their own
sake. The reason it mattered is that `docs/BUDGET.md` tracks consumption across
milestones, and a metric with silent hash-dependent noise in it cannot support
the claim that a milestone stayed within its line. RAM is the resource under
real pressure, so the same question should be asked of it before M4 leans on
these numbers: confirm that `.data` and `.bss` figures are stable under
irrelevant changes, not merely small.

---

## 2026-08-20 — M0 revision: measurement convention, stack sizing, run termination

**Measured commit:** `dc918ca244fefd5e695f811aa5d13ffe1017432c`
**Toolchain:** GCC 14.2.0, QEMU 10.2.1

This entry records a measurement of the commit above, not of itself. From now on
a reference figure is only taken on a clean tree: `make measure` refuses to run
when `git status --porcelain` is non-empty. A `-dirty` hash names a state nobody
else can check out, which is worthless in a project whose claim is
reproducibility. The banner keeps `git describe --always --dirty` because that
is a build artefact, not a reference document.

The figures in the M0 entry below were taken on a dirty tree under the old
convention. They are superseded by this entry.

### Two corrections to yesterday's conclusions

Both were my errors, and both were checked rather than argued.

**A RISC-V GDB is packaged; the host had the wrong one.** I concluded that no
RISC-V GDB was available and that one would have to be built from source. Wrong
inference. `gdb --configuration` reports
`--host=x86_64-linux-gnu --target=x86_64-linux-gnu`: the *native* binary is
x86-only. `gdb-multiarch` carries the RISC-V targets. The fix is an `apt
install` and setting the architecture explicitly before `target remote`.

**Semihosting gives an exit status without finisher hardware.** Verified rather
than assumed: a minimal image issuing `SYS_EXIT_EXTENDED` with a parameter block
of `{0x20026, 42}`, run under `-semihosting-config enable=on,target=native`,
makes QEMU exit with status 42 immediately.

```
CODE DE SORTIE QEMU = 42
```

It is not used in the flight image and must never be. A call gate from firmware
into the host is a clean exit path in emulation and a defect on a vehicle; if it
is ever compiled in, it goes behind a test-only build flag that makes shipping
it impossible rather than unlikely.

### Stack sized from measurement

The boot path peaks at **64 bytes**. Read back at run time by walking the
`0xDEADBEEF` paint from `__stack_bottom`, and now reported on every banner
rather than measured once and forgotten.

The reservation was 4096 B, sixteen times more than anything observed, on a
board with 16384 B in total. Reduced to 1024 B. RAM consumption falls from
4096 B to 1024 B, returning 3072 B — nineteen percent of all the RAM in the
system — to the budget, where M4's triple-redundant state is the intended
beneficiary.

Two things keep this honest going forward: the link script fails the build on
RAM overflow, and the banner reports live high-water on every boot. Re-measure
at M2 under scheduler load; 64 B is a boot-path figure, not a steady-state one.

### Run termination fixed

Runs were ending on a 10 s timeout because the firmware parks forever and
`sifive_e` has no test finisher. The host now watches the serial stream and
stops QEMU on the sentinel `boot   : ok`. The smoke test goes from **10 s to
0.8 s**. The timeout survives as the failure path, which is what it should have
been all along.

This matters beyond convenience: M10 calls for 1000 consecutive runs. At the old
cost that is just under three hours of waiting for timeouts. At the new one it
is roughly thirteen minutes.

### Measurements

`make measure` on `dc918ca`:

```
   text    data     bss     dec     hex filename
    826       0    1024    1850     73a build/obc.elf
```

```
=== OBC-Zero ===
build  : dc918ca
board  : sifive_e
entry  : 0x2040008C
ram    : 1024 B of 16384 B
stack  : 64 B peak of 1024 B reserved
boot   : ok
```

Flash is 826 B against a 4 MiB budget. RAM is 1024 B of 16384 B, 6.3 percent,
down from 25 percent.

**Correction, same day.** I attributed the 12 B shrink to the clean build hash
being a shorter string than the dirty one. That is wrong: both are seven
characters. The real cause is linker string tail-merging, and it turned out to
matter more than the number did. See the entry above dated the same day.

### Also done

`docs/BUDGET.md` written: the 16 KiB is now allocated across M2, M3, M4, M6, M7
and M8 in advance, with a 3328 B reserve held back and assigned to nothing.
Estimates other than the stack are unmeasured, and the reserve exists because
M4 and M5 are exactly the kind of work that discovers a memory need late.

Noted into M5: **the idle loop must never feed the watchdog.** A system whose
tasks have all died would otherwise look healthy indefinitely. The feed is
earned by tasks completing, not by the core being alive.

### Carried into M9, to settle first

The gdbstub attaches in wall-clock time, not instruction count. M9 requires
injectors deterministic given a seed, but a host attaching after a wall-clock
delay lands on a different instruction every run. Determinism needs a breakpoint
on an instruction counter or a TCG plugin. **Decide at the start of M9**: it
determines the architecture of `harness/faults/` and cannot be retrofitted.

---

## 2026-08-20 — M0: scaffold and first boot

**Outcome:** M0 complete. The image boots under QEMU and identifies itself.

### What was done

Established the target platform empirically before writing any code, then built
the minimum that boots: link script, reset code, transmit-only UART, banner.

### Platform findings

The device tree dump prescribed by the plan does not work. `sifive_e` has no
FDT at all:

```
qemu-system-riscv32: This machine doesn't have an FDT
```

Substituted `info mtree` from the QEMU monitor, which reports the memory map of
the machine as instantiated. Output checked in at `emu/sifive_e-mtree.txt`.
The plan has been corrected accordingly.

Reset behaviour read out of the halted machine rather than assumed. `pc` at
reset is `0x00001004`; the mask ROM there contains exactly two instructions,
`lui t0,0x20400` then `jr t0`. So the entry point is `0x20400000`, in XIP flash.
No bootloader, no SBI.

Three constraints that shape everything after this milestone:

- **RAM is 16 KiB.** `0x80000000`–`0x80003fff`, and that is all there is. It
  holds `.data`, `.bss` and the stack. Currently 4 KiB stack, 0 B of `.data`
  and `.bss`, leaving 12 KiB for every later milestone to share.
- **Execution is XIP**, so `.data` genuinely has to be copied from flash to RAM
  at boot. The link script asserts on RAM overflow at link time.
- **There is no test finisher device.** The firmware cannot exit QEMU with a
  status code. M9's runner will have to end runs from the host side. Recorded
  in ADR 0001 so it is not rediscovered at M9.

Two toolchain findings:

- `_zicsr` must be spelled out in `-march`. `binutils` 2.45 rejects `csrw`
  under a bare `rv32imac`; the flag is now `-march=rv32imac_zicsr`.
- `-mcmodel=medany` is mandatory. `medlow` cannot reach `0x80000000`.

### Measurements

**Superseded by the entry above.** These were taken on a dirty tree, before the
clean-tree measurement rule existed, so the hash below names nothing checkoutable.
Kept because deleting a superseded measurement is how a logbook stops being one.

`riscv64-unknown-elf-size build/obc.elf`, build `a196c30c6bce-dirty`:

```
   text    data     bss     dec     hex filename
    706       0    4096    4802    12c2 build/obc.elf
```

`bss` is the 4 KiB stack reservation; actual `.bss` is 0 bytes. Flash footprint
is 706 B of the 4 MiB budgeted. RAM footprint is 4096 B of 16384 B, 25 percent
consumed before a single feature exists.

### Boot output

```
=== OBC-Zero ===
build  : a196c30c6bce-dirty
board  : sifive_e
entry  : 0x2040008C
ram    : 4096 B of 16384 B
boot   : ok
```

Build clean at `-Wall -Wextra -Werror`, with `-Wconversion -Wsign-conversion
-Wshadow -Wundef` added on top. ELF entry point is `0x20400000`, matching the
mask ROM jump target.

### Open points carried forward

- ~~**`make gdb` is only half usable.** No RISC-V GDB is packaged on this
  host.~~ **Wrong, corrected in the entry above.** `gdb-multiarch` carries the
  RISC-V targets; only the *native* `gdb` binary is x86-only.
- ~~**`make test` takes 10 s.**~~ **Fixed in the entry above**, 0.8 s via a
  serial sentinel.
- **The idle loop in `obc_main` is unbounded**, which sits against the "every
  loop has a provable bound" rule. An idle park is the intended exception, but
  it is the only one and should stay that way. M1 replaces it with the
  scheduler idle path, and M5 must ensure it never feeds the watchdog.
- **`mtimecmp` and `mtime` offsets are assumed**, not verified. Standard SiFive
  layout puts them at `0x02004000` and `0x0200bff8` within the MTIMER region.
  Confirm against the machine at M1 before relying on them.

### Not done, deliberately

Interrupts, timers, traps, tasks, and any test beyond "it boots" are out of M0
scope and were not started.
