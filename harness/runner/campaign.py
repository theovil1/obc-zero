"""Run a seeded fault-injection campaign, with a live view of what it is doing.

Watching a progress bar is not the point. The point is that a campaign takes
long enough that the only alternative to watching it is not watching it, and a
campaign nobody watches is one where a systematic failure — every injection into
one copy, every flip of one bit — is discovered at the end instead of at the
tenth run.

So the breakdown is live: per copy, per corruption depth, and the failures as
they happen.

Each run gets its own gdbstub port. Every injector before this one hardcoded
1234, which meant two runs could not coexist: the second QEMU failed to bind,
the debugger attached to the first one's target, and the failures named
subsystems that were working. That is recorded in the backlog as an M9
requirement and is fixed here because a half-hour campaign that blocks every
other test is a campaign nobody runs twice.

Copyright 2026 Théo Vilain
SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import argparse
import random
import re
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

QEMU = "qemu-system-riscv32"
GDB = "gdb-multiarch"
MACHINE = "sifive_e"
ICOUNT = "shift=6"
SENTINEL = "boot   : ok"
STATE_RE = re.compile(r"repairs=(\d+) unresolved=(\d+) mode=(\d+)")

COPY_NAMES = ("a", "b", "c")


@dataclass(frozen=True)
class Injection:
    """One planned corruption, derived from the campaign seed."""

    index: int
    copies: int
    first: int
    bit: int

    def describe(self) -> str:
        names = "+".join(COPY_NAMES[(self.first + i) % 3] for i in range(self.copies))
        return f"{names} bit {self.bit}"


@dataclass(frozen=True)
class Verdict:
    """What one run produced, and whether it is what the voter promises."""

    injection: Injection
    ok: bool
    repairs: int
    unresolved: int
    mode: int
    reason: str


@dataclass
class Tally:
    """Running totals, kept per dimension so a systematic bias shows up early."""

    done: int = 0
    failures: list[Verdict] = field(default_factory=list)
    by_copy: dict[str, int] = field(default_factory=dict)
    by_depth: dict[int, int] = field(default_factory=dict)
    repaired: int = 0
    unresolved: int = 0


def free_port() -> int:
    """Ask the kernel for a port nobody is using, and hand it straight over.

    There is a race between closing this socket and QEMU binding it. It is
    accepted deliberately: the alternative is a fixed port, and a fixed port is
    what made six passing subsystems report failures.
    """
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def plan(runs: int, seed: int) -> list[Injection]:
    """The injection plan, reproducible from the seed alone.

    Generated up front rather than as the campaign goes, so that a failing run
    can be re-run on its own without replaying everything before it.
    """
    rng = random.Random(seed)
    return [
        Injection(
            index=i,
            copies=rng.randint(1, 2),
            first=rng.randint(0, 2),
            bit=rng.randint(0, 31),
        )
        for i in range(runs)
    ]


def run_one(elf: Path, build: Path, inj: Injection, timeout: float) -> Verdict:
    """Boot the image, corrupt the copies, and read back what the voter did."""
    port = free_port()
    serial = build / f"campaign-serial-{inj.index}.log"
    serial.write_text("", encoding="utf-8")

    qemu = subprocess.Popen(
        [
            QEMU,
            "-machine",
            MACHINE,
            "-icount",
            ICOUNT,
            "-display",
            "none",
            "-serial",
            f"file:{serial}",
            "-kernel",
            str(elf),
            "-gdb",
            f"tcp::{port}",
            "-S",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.6)
        inject = [
            GDB,
            str(elf),
            "-batch",
            "-ex",
            f"set $critical_copies = {inj.copies}",
            "-ex",
            f"set $critical_first = {inj.first}",
            "-ex",
            f"set $critical_bit = {inj.bit}",
            "-ex",
            f"set $campaign_port = {port}",
            "-x",
            "harness/faults/critical.gdb",
        ]
        # check=False throughout: a debugger that exits non-zero is a data
        # point about the run, not a reason to abandon the campaign.
        out = subprocess.run(
            inject, capture_output=True, text=True, timeout=timeout, check=False
        ).stdout
        if "CRITICAL-INJECTED" not in out:
            return Verdict(inj, False, 0, 0, 0, "nothing was injected")

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if SENTINEL in serial.read_text(encoding="utf-8", errors="replace"):
                break
            time.sleep(0.02)

        read_back = subprocess.run(
            [
                GDB,
                str(elf),
                "-batch",
                "-ex",
                "set confirm off",
                "-ex",
                "set architecture riscv:rv32",
                "-ex",
                f"target remote localhost:{port}",
                "-ex",
                (
                    'printf "repairs=%u unresolved=%u mode=%u\\n", '
                    "obc_critical_repairs, obc_critical_unresolved, obc_mode"
                ),
            ],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        ).stdout
    except subprocess.TimeoutExpired:
        return Verdict(inj, False, 0, 0, 0, "timed out")
    finally:
        qemu.kill()
        qemu.wait()
        serial.unlink(missing_ok=True)

    found = STATE_RE.search(read_back)
    if not found:
        return Verdict(inj, False, 0, 0, 0, "could not read the voter back")

    repairs, unresolved, mode = (int(g) for g in found.groups())

    # One corrupted copy must leave a majority, so it is repaired and nothing
    # about the system's behaviour changes. Two must not produce a verdict at
    # all, and the caller must fail safe rather than trust a survivor.
    if inj.copies == 1:
        ok = repairs >= 1 and unresolved == 0 and mode == 0
        reason = "" if ok else "a single corruption was not silently repaired"
    else:
        ok = unresolved >= 1 and mode == 1
        reason = "" if ok else "two corruptions still produced a verdict"

    return Verdict(inj, ok, repairs, unresolved, mode, reason)


def bar(done: int, total: int, width: int = 34) -> str:
    """A progress bar that degrades to something readable in a log file."""
    filled = 0 if total == 0 else int(width * done / total)
    return "█" * filled + "░" * (width - filled)


def render(tally: Tally, total: int, started: float, live: bool) -> str:
    """The frame. Kept to a fixed number of lines so it can redraw in place."""
    elapsed = time.monotonic() - started
    rate = tally.done / elapsed if elapsed > 0 else 0.0
    remaining = (total - tally.done) / rate if rate > 0 else 0.0
    passed = tally.done - len(tally.failures)

    def clock(seconds: float) -> str:
        return f"{int(seconds) // 60:d}m{int(seconds) % 60:02d}s"

    copies = "  ".join(f"{name} {tally.by_copy.get(name, 0):>4}" for name in COPY_NAMES)
    depth = "  ".join(
        f"{n} copy {tally.by_depth.get(n, 0):>4}"
        if n == 1
        else f"{n} copies {tally.by_depth.get(n, 0):>4}"
        for n in (1, 2)
    )

    lines = [
        "  voter campaign",
        (
            f"  {bar(tally.done, total)}  {tally.done:>4}/{total}"
            f"   {100 * tally.done / total if total else 0:5.1f}%"
        ),
        "",
        f"  passed   {passed:>5}          elapsed  {clock(elapsed):>8}",
        f"  failed   {len(tally.failures):>5}          remaining{clock(remaining):>8}",
        f"  rate     {rate:>5.2f}/s        repairs  {tally.repaired:>8}",
        f"  {'':>5}                unresolved{tally.unresolved:>7}",
        "",
        f"  corrupted copy   {copies}",
        f"  corruption depth {depth}",
        "",
    ]

    if tally.failures:
        lines.append("  failures")
        for verdict in tally.failures[-4:]:
            lines.append(
                f"    run {verdict.injection.index:>4}  "
                f"{verdict.injection.describe():<14} {verdict.reason}"
            )
        lines.extend([""] * max(0, 5 - len(tally.failures[-4:])))
    else:
        lines.extend(["  failures", "    none", "", "", "", ""])

    frame = "\n".join(lines)
    return frame if live else frame + "\n"


def write_report(
    path: Path, tally: Tally, total: int, seed: int, elapsed: float, commit: str
) -> None:
    """A report that names its seed, because one that does not is an anecdote."""
    lines = [
        f"# Voter campaign, {total} randomised corruptions",
        "",
        f"- **Date:** {time.strftime('%Y-%m-%d')}",
        f"- **Commit:** `{commit}`",
        f"- **Seed:** `{seed}` — the campaign replays exactly from this alone",
        f"- **Runs:** {total}",
        f"- **Failures:** {len(tally.failures)}",
        f"- **Wall clock:** {elapsed / 60:.1f} minutes",
        "",
        "## What each run asserts",
        "",
        "One corrupted copy must leave a majority, be repaired, and change",
        "nothing about the system's behaviour. Two must produce no verdict at",
        "all, and the caller must fail safe rather than trust a survivor. A run",
        "that injects nothing is a failure, not a pass.",
        "",
        "## Coverage",
        "",
        "| Dimension | Count |",
        "|---|---:|",
    ]
    for name in COPY_NAMES:
        lines.append(f"| first copy `{name}` | {tally.by_copy.get(name, 0)} |")
    for n in (1, 2):
        lines.append(f"| {n} copy corrupted | {tally.by_depth.get(n, 0)} |")
    lines += [
        f"| repairs performed | {tally.repaired} |",
        f"| votes unresolved | {tally.unresolved} |",
        "",
    ]

    if tally.failures:
        lines += ["## Failures", "", "| Run | Injection | Reason |", "|---|---|---|"]
        lines += [
            f"| {v.injection.index} | {v.injection.describe()} | {v.reason} |"
            for v in tally.failures
        ]
    else:
        lines += [
            "## Failures",
            "",
            "None.",
            "",
            "That is what the voter is supposed to do, and it is worth saying",
            "plainly what this does **not** show: the injections are single and",
            "double bit flips in the stored value, applied before the window",
            "opens. Corrupting a checksum word, corrupting mid-window, and",
            "corrupting three copies at once are different faults and are not",
            "covered here.",
        ]
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main(argv: list[str]) -> int:
    """Run the campaign. Returns a process exit status."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runs", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--elf", type=Path, default=Path("build/obc.elf"))
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--report", type=Path, default=None)
    parser.add_argument("--timeout", type=float, default=40.0)
    parser.add_argument(
        "--status",
        type=Path,
        default=Path("build/campaign-status.txt"),
        help=(
            "file rewritten with the current frame on every run. Redirecting "
            "stdout to a log appends frames instead of redrawing them, so "
            "following that log scrolls a wall of stale dashboards. This holds "
            "one frame and only ever the latest."
        ),
    )
    args = parser.parse_args(argv[1:])

    if not args.elf.exists():
        print(f"no image at {args.elf}; run 'make build' first", file=sys.stderr)
        return 2

    commit = subprocess.run(
        ["git", "describe", "--always", "--dirty"],
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()

    # Resolved and checked before a single run, not after fourteen minutes of
    # them. A refusal that arrives at the end has already wasted the campaign it
    # refused to record.
    report = args.report or Path(
        f"docs/reports/{time.strftime('%Y-%m-%d')}-voter-campaign-{commit}.md"
    )
    if report.exists():
        print(
            f"  REFUSED: {report} already exists.\n"
            "  Reports are append-only history. Pass --report to write "
            "elsewhere, or\n  re-run against a different commit.",
            file=sys.stderr,
        )
        return 2

    injections = plan(args.runs, args.seed)
    tally = Tally()
    started = time.monotonic()
    live = sys.stdout.isatty()
    height = 0

    print(f"  seed {args.seed}, image {args.elf}, commit {commit}")
    if not live and args.status:
        print(f"  watch it with:  watch -n 1 -t cat {args.status}")
    print()

    for inj in injections:
        verdict = run_one(args.elf, args.build, inj, args.timeout)

        tally.done += 1
        name = COPY_NAMES[inj.first]
        tally.by_copy[name] = tally.by_copy.get(name, 0) + 1
        tally.by_depth[inj.copies] = tally.by_depth.get(inj.copies, 0) + 1
        tally.repaired += verdict.repairs
        tally.unresolved += verdict.unresolved
        if not verdict.ok:
            tally.failures.append(verdict)

        frame = render(tally, args.runs, started, live)

        # Always, whether or not stdout is a terminal. A campaign launched in
        # the background is exactly the one somebody wants to watch, and it is
        # the one whose stdout is a file.
        if args.status:
            args.status.parent.mkdir(parents=True, exist_ok=True)
            temporary = args.status.with_suffix(".tmp")
            temporary.write_text(frame + "\n", encoding="utf-8")
            temporary.replace(args.status)

        if live:
            if height:
                sys.stdout.write(f"\033[{height}A")
            sys.stdout.write("\033[J" + frame + "\n")
            height = frame.count("\n") + 1
            sys.stdout.flush()
        elif tally.done % 20 == 0 or not verdict.ok or tally.done == args.runs:
            print(frame, flush=True)

    elapsed = time.monotonic() - started
    report.parent.mkdir(parents=True, exist_ok=True)
    write_report(report, tally, args.runs, args.seed, elapsed, commit)

    print(f"\n  report written to {report}")
    print(f"  reproduce with: --runs {args.runs} --seed {args.seed}")
    return 1 if tally.failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
