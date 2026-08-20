# ADR 0001: Target platform and memory map

- **Status:** accepted
- **Date:** 2026-08-20
- **Milestone:** M0

## Context

The flight software needs a fixed target before any address can be written into
a linker script or a driver. `README.md` commits the project to RV32 under QEMU
`sifive_e`, but that name alone does not determine the reset behaviour, the
load address, or the size of RAM, all of which shape the boot code.

This record fixes those values and states where each one came from. Addresses
were not taken from documentation, from memory, or from published examples.

## Decision

Target the QEMU `sifive_e` machine, RV32IMAC, machine mode only, executing in
place from XIP flash with data and stack in DTIM RAM.

## How the values were obtained

`sifive_e` exposes **no FDT**. The device tree dump prescribed by the original
plan fails outright:

```
$ qemu-system-riscv32 -machine sifive_e,dumpdtb=emu/sifive_e.dtb -nographic
qemu-system-riscv32: This machine doesn't have an FDT
```

The equivalent authoritative source is the QEMU monitor, which reports the
memory map of the machine as actually instantiated. The full output is checked
in at `emu/sifive_e-mtree.txt`, with the command needed to regenerate it:

```
printf 'info mtree\nquit\n' | qemu-system-riscv32 -machine sifive_e \
  -display none -serial null -monitor stdio -S
```

The reset behaviour was read out of the machine rather than assumed. Halted at
reset, the core reports `pc = 0x00001004`, inside the mask ROM. Dumping and
disassembling that ROM gives the whole of its content:

```
1004:   204002b7    lui  t0,0x20400
1008:   00028067    jr   t0            # 0x20400000
```

The mask ROM therefore performs an unconditional jump to `0x20400000` and does
nothing else. There is no bootloader, no SBI, and no argument passing.

## Memory map

Recorded from `emu/sifive_e-mtree.txt`, QEMU 10.2.1.

| Region | Range | Size | Used at M0 |
|---|---|---|---|
| Mask ROM | `0x00001000`–`0x00002fff` | 8 KiB | Reset vector only |
| ACLINT SWI (`msip`) | `0x02000000`–`0x02003fff` | 16 KiB | No |
| ACLINT MTIMER | `0x02004000`–`0x0200bfff` | 32 KiB | No, M1 |
| PLIC | `0x0c000000`–`0x0fffffff` | 64 MiB | No |
| AON | `0x10000000`–`0x1000014f` | 336 B | No |
| PRCI | `0x10008000`–`0x10008fff` | 4 KiB | No |
| GPIO | `0x10012000`–`0x100120ff` | 256 B | No |
| **UART0** | `0x10013000`–`0x1001301f` | 32 B | **Yes** |
| UART1 | `0x10023000`–`0x1002301f` | 32 B | No |
| QSPI0/1/2, PWM0/1/2 | `0x10014000`+ | — | No |
| **XIP flash** | `0x20000000`–`0x3fffffff` | 512 MiB | **Yes, from `0x20400000`** |
| **DTIM RAM** | `0x80000000`–`0x80003fff` | **16 KiB** | **Yes** |

Within the MTIMER region the standard SiFive layout places `mtimecmp` at
`0x02004000` and `mtime` at `0x0200bff8`. This has not been verified against the
machine and must be confirmed at M1 before it is relied upon.

## Consequences

**Execution is XIP.** Code and read-only data are linked at `0x20400000` and
execute from flash. `.data` has a load address in flash and a run address in
RAM, so the startup code must copy it. This is why `.data` copy is part of M0
rather than an optimisation.

**RAM is 16 KiB, and that is the binding constraint of the whole project.** It
holds `.data`, `.bss`, and the stack, and nothing else. Every later milestone
competes for it: triple-redundant state (M4), telemetry buffers (M6), the
command queue (M7) and the event log (M8) all live here. The split between them
is fixed in advance in `docs/BUDGET.md` rather than settled by whichever
subsystem is written first. The link script asserts on total overflow, so
exceeding the budget is a build failure rather than a run-time surprise.

The flash region is capped at 4 MiB in the link script rather than the 512 MiB
the emulator offers. The 512 MiB window is an emulator artefact; the HiFive1
this machine models carries 16 MiB. Linking against the emulator's generosity
would produce images that cannot exist on the hardware targeted in Phase 2.

**`medany` code model is mandatory.** `medlow` addresses ±2 GiB around zero, and
`0x80000000` falls outside that range. Building with `medlow` fails at link time.

**`_zicsr` must be named explicitly in `-march`.** CSR access became a separate
extension when the ISA specification was split, and binutils 2.45 rejects `csrw`
under a bare `rv32imac`. `-march=rv32imac_zicsr` adds no capability the hardware
lacks; it only names an extension that was previously implicit.

**There is no test finisher device.** Unlike the `virt` machine, `sifive_e`
offers no MMIO register by which the firmware can terminate the emulator with an
exit status. Two mechanisms cover this, and the choice between them is a safety
decision rather than a convenience one.

*Serial sentinel, used now.* The firmware emits a known string when a run
reaches its end state and the host stops QEMU on seeing it. This costs the
flight image nothing it was not already doing, and is what `make test` uses.

*RISC-V semihosting, available but gated.* QEMU implements it on this machine.
Verified directly: an image issuing `SYS_EXIT_EXTENDED` with a parameter block
of `{0x20026, 42}` under `-semihosting-config enable=on,target=native` makes
QEMU exit with status 42, immediately and with no finisher hardware.

Semihosting must be compiled in **behind a test-only build flag and must never
appear in a flight image**. It is a call gate from the firmware into the host:
in an emulated test that is a clean exit path, on a real vehicle it is a defect
with no defensible purpose. The build must make it impossible to ship by
accident, not merely unlikely.

**A RISC-V capable GDB is packaged, the host simply has the wrong one.**
`gdb-multiarch` includes the RISC-V targets. The native `gdb` reports
`configure --host=x86_64-linux-gnu --target=x86_64-linux-gnu`, which is why it
rejects `set architecture riscv:rv32`. The fix is `apt install gdb-multiarch`
and setting the architecture explicitly before `target remote`; nothing needs to
be built from source.

**The gdbstub attaches in wall-clock time, not instruction count.** This is the
real constraint on M9, and it is sharper than the packaging question. M9 requires
injectors that are deterministic given a seed, but a host that attaches and
corrupts memory after a wall-clock delay will land on a different instruction
every run. Determinism therefore needs either a breakpoint on an instruction
counter or a TCG plugin driving the injection. **This must be settled at the
start of M9, before any injector is written**, because it decides the whole
architecture of `harness/faults/` rather than being fixable afterwards.

## Alternatives considered

**QEMU `virt`.** Has an FDT, a test finisher, and configurable RAM, all of which
would make the harness easier to build. Rejected: `virt` is a hypervisor-style
machine with a comfortable memory model, and designing against it would hide the
16 KiB constraint that makes this project a realistic flight-software exercise.
The difficulty is the point.

**Linking into RAM and loading with `-kernel` at `0x80000000`.** Would avoid the
`.data` copy. Rejected: it contradicts the mask ROM, which jumps to
`0x20400000` regardless, and it would make the emulated boot path differ from
any real one.

## Verification

This record holds decisions and the platform facts behind them. It deliberately
holds **no measurements**: figures belong in `docs/LOGBOOK.md`, against the
commit they were taken on. A decision record that quotes its own build numbers
goes stale the first time anything is rebuilt.

The decisions above are exercised by every build. The ELF entry point must be
`0x20400000`, matching the mask ROM jump target, and the link script asserts
that `.data`, `.bss` and the stack fit in 16 KiB. Both are checked by
`make measure`, which refuses to run on a dirty tree so that the figures it
prints always name a commit that can be checked out.
