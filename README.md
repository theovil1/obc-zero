# OBC-Zero

An open, ITAR-free on-board computer stack for small spacecraft, built on RISC-V.

OBC-Zero is a bare-metal flight software core and a fault-injection test harness.
It starts as a fully emulated system: no hardware is required to run, test, or
contribute to it. The goal is a flight computer whose survival behaviour is
measured and published, not asserted.

## Why this exists

European spacecraft integrators depend heavily on US-controlled avionics.
Export control regimes constrain what can be bought, when, and by whom, and the
few available alternatives are expensive and closed. OBC-Zero is an attempt to
build the software half of a sovereign alternative in the open, on an open ISA,
with an auditable record of what was tested and what failed.

## Status

Pre-alpha. Nothing here has flown. Nothing here is qualified.
Treat every claim in `docs/` as a lab result, not a flight heritage claim.

**Milestone 0 complete.** The image boots on QEMU `sifive_e` and reports its
build, entry point and memory footprint. Current figures, measured on a clean
tree and pinned against drift:

| | |
|---|---|
| Flash | 874 B of a 4 MiB budget |
| RAM | 1024 B of **16384 B**, the whole of the board's memory |
| Stack peak | 64 B, measured from a paint pattern at run time |
| Boot to banner | under 0.1 s |

Every address the image relies on was read out of the machine rather than taken
from documentation, and the reset vector was confirmed independently through the
debugger. The reasoning is in `docs/adr/`, the measurements in
`docs/LOGBOOK.md`, and the RAM allocation for later milestones in
`docs/BUDGET.md`.

Fault injection does not exist yet. Until it does, this repository demonstrates
nothing about fault tolerance, which is the only thing it is ultimately for.

## Scope

An on-board computer does four things. OBC-Zero implements them in this order:

1. **Acquire** telemetry from sensors (temperature, bus voltage, attitude).
2. **Execute** time-tagged commands received from the ground.
3. **Store** state and events for later downlink.
4. **Survive** its own faults: detect corruption, latch-ups, hangs, and brownouts,
   then recover to a known-good state without ground intervention.

Point 4 is the product. Points 1 to 3 are table stakes.

## Non-goals

- Attitude determination and control algorithms.
- Radio, modem, or RF link layer.
- Ground segment software. Use Yamcs or OpenC3.
- Hard real-time guarantees on general-purpose Linux.

## Target platform

| Layer | Choice | Rationale |
|---|---|---|
| ISA | RV32IMAC | Open ISA, no export restriction, EU investment path |
| Emulated board | QEMU `sifive_e` | Bare metal, no MMU, close to a real flight MCU |
| Language | C11, freestanding | Auditable, no runtime, standard practice in flight software |
| Host tooling | Python 3.11+ | Fault injection harness and report generation |

## Quick start

Ubuntu 24.04:

```bash
sudo apt install gcc-riscv64-unknown-elf qemu-system-riscv \
                 device-tree-compiler gdb-multiarch make python3-venv
git clone https://github.com/<user>/obc-zero.git
cd obc-zero
make build          # produces build/obc.elf
make run            # boots the image under QEMU, Ctrl-A X to exit
make test           # smoke test: the image boots and identifies itself
make measure        # reference measurement; refuses to run on a dirty tree
make gdb            # QEMU halted at reset with a gdbstub on :1234
make attach         # attach gdb-multiarch to a `make gdb` in another shell
```

`make fault` is not implemented yet. The fault injection campaign arrives with
the harness; until then there is no campaign to run and this README will not
pretend otherwise.

Everything runs under `-icount shift=0`, which makes guest execution
deterministic. That is what allows a per-task budget to mean anything and a
seeded fault injector to land on the same instruction every run. See
`docs/adr/0002-time-domains.md`.

## Repository layout

```
flight/            Bare-metal flight software. No host dependencies.
  boot/            Reset vector, stack setup, .bss zeroing, .data copy
  core/            Scheduler, watchdog, state machine, recovery logic
  hal/             Register-level device access, sifive_e only
  tlm/             Telemetry acquisition and packing
  cmd/             Command ingest, validation, time-tagged execution
  store/           Non-volatile state, event log, redundant copies
harness/           Host-side test and fault injection harness (Python).
  runner/          QEMU orchestration, serial capture, run lifecycle
  faults/          Injectors: bit flip, hang, brownout, sensor faults
  report/          Report generation into docs/reports/
emu/               QEMU machine configuration and linker scripts.
docs/
  adr/             Architecture decision records, numbered, immutable
  reports/         Dated campaign reports, never deleted
  LOGBOOK.md       Engineering logbook, newest entry at the top
```

`flight/core/` is the part that matters. Changes there require a matching
fault injection test, as described in `CONTRIBUTING.md`.

## Test philosophy

A flight computer is credible only through evidence. Every campaign in this
repository produces a dated report in `docs/reports/`, including the runs that
failed. Failed runs are never deleted, only superseded.

The escalation ladder, in order:

1. Emulated fault injection (bit flips, hangs, power loss, lying sensors).
2. Long-duration soak runs under continuous fault pressure.
3. Thermal cycling on real hardware.
4. Vacuum exposure on real hardware.
5. Radiation testing at a facility with beam time.
6. Stratospheric balloon flight.
7. Orbital flight as a secondary payload.

Stages 1 and 2 require nothing but time. The project stays there until they are
exhausted.

## Licence

Apache License 2.0. See `LICENSE`.

The harness and certification tooling may be licensed separately in the future.
The flight core will remain Apache 2.0.

## Author

Théo Vilain
