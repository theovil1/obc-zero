# Engineering logbook

Newest entry at the top. One entry per working session. Record what was
measured, not what was intended.

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

- **`make gdb` is only half usable.** No RISC-V GDB is packaged on this host:
  neither `riscv64-unknown-elf-gdb` nor `gdb-multiarch` is installed, and the
  native `gdb` does not know `riscv:rv32`. QEMU's gdbstub starts correctly, so
  the target is not wrong, but nothing can attach to it yet. M9 needs the
  gdbstub for fault injection, so this must be resolved before then.
- **`make test` takes 10 s** because the firmware parks forever and the run is
  ended by a timeout. Acceptable for one smoke test, not for the 1000-run
  campaign of M10. The real fix is the host-side termination mechanism above.
- **The idle loop in `obc_main` is unbounded**, which sits against the "every
  loop has a provable bound" rule. An idle park is the intended exception, but
  it is the only one and should stay that way. M1 replaces it with the
  scheduler idle path.
- **`mtimecmp` and `mtime` offsets are assumed**, not verified. Standard SiFive
  layout puts them at `0x02004000` and `0x0200bff8` within the MTIMER region.
  Confirm against the machine at M1 before relying on them.

### Not done, deliberately

Interrupts, timers, traps, tasks, and any test beyond "it boots" are out of M0
scope and were not started.
