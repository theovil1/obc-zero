# Engineering logbook

Newest entry at the top. One entry per working session. Record what was
measured, not what was intended.

---

## 2026-08-20 — Footprint was not a stable metric

**Measured commit:** `578246b2e34910e75d70a888a4c1b11e16941079`
**Toolchain:** GCC 14.2.0, QEMU 10.2.1

Found while checking an unexplained 4-byte discrepancy between two builds. Worth
the detour: the metric `docs/BUDGET.md` is built on was moving on its own.

### What happened

`58199cd` measured 830 B of text; its parent `0205847` measured 826 B. `58199cd`
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
| `0205847` | 826 | 184 |
| `58199cd` | 830 | 188 |
| `aaaaaaa` | 830 | 188 |
| `bbbbbbb` | 830 | 188 |
| `zzzzzzz` | 830 | 188 |

`0205847` is the outlier: it got a merge the others did not. The 826 B recorded
as the M0 reference was a lucky hash, not a smaller program.

### Fix

The hash now lives in its own section instead of being concatenated into a
string literal, so it never enters the mergeable pool. Verified across six
different hashes, all producing an identical image size:

```
hash=0205847  text=858  rodata=188
hash=58199cd  text=858  rodata=188
hash=aaaaaaa  text=858  rodata=188
hash=bbbbbbb  text=858  rodata=188
hash=zzzzzzz  text=858  rodata=188
hash=deadbee  text=858  rodata=188
```

Costs 28 B of flash against a 4 MiB budget, and buys a number that means
something when compared across commits.

### Measurement

`make measure` on `578246b`:

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

**Measured commit:** `02058475c3636274fc4db1156280a890ced00840`
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

`make measure` on `0205847`:

```
   text    data     bss     dec     hex filename
    826       0    1024    1850     73a build/obc.elf
```

```
=== OBC-Zero ===
build  : 0205847
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

`riscv64-unknown-elf-size build/obc.elf`, build `d668b4d1de0f-dirty`:

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
build  : d668b4d1de0f-dirty
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
