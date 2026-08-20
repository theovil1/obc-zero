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
holds `.data`, `.bss`, and the stack, and nothing else. The current allocation
is 4 KiB of stack and 0 bytes of `.data`/`.bss`. Every later milestone competes
for the remaining 12 KiB: triple-redundant state (M4), the event log (M8), and
telemetry buffers (M6) all live here. The link script asserts on overflow so
that exceeding it is a build failure rather than a run-time surprise.

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
exit status. The harness cannot rely on a process exit code to decide pass or
fail, and must instead drive run termination from the host side, through the
gdbstub, a timeout, or a sentinel in the serial stream. This constrains the
design of `harness/runner/` at M9 and is recorded here so the constraint is not
discovered then.

**No RISC-V GDB is packaged on the development host.** Neither
`riscv64-unknown-elf-gdb` nor `gdb-multiarch` ships with the Ubuntu packages
installed, and the native `gdb` does not know the `riscv:rv32` architecture.
`make gdb` starts QEMU with `-s -S` and is therefore only half usable until a
debugger is installed. M9 depends on the gdbstub for fault injection, so this
must be resolved before then; it does not block M0 through M8.

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

The values in this record are exercised by the M0 build and boot:

```
$ make build
   text    data     bss     dec     hex filename
    706       0    4096    4802    12c2 build/obc.elf

$ make test
=== OBC-Zero ===
build  : d668b4d1de0f-dirty
board  : sifive_e
entry  : 0x2040008C
ram    : 4096 B of 16384 B
boot   : ok
PASS
```

The ELF entry point is `0x20400000`, matching the mask ROM jump target exactly.
