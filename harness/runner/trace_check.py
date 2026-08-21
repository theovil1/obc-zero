"""Assert the scheduler's conformance, ordering and reproducibility.

What is checked here is deliberately not "the tasks ran". It is that the order
and the number of executions within a window are exactly those the task table
dictates, and that they do not change between runs. See
``docs/adr/0003-scheduler-observability.md``.

Every expectation is derived from the task table dumped out of the binary under
test. Restating the periods here would mean the assertion checks this file's
opinion rather than the flight software's table, and the two would drift.

Copyright 2026 Théo Vilain
SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Task:
    """One row of the task table, as declared in the image under test."""

    index: int
    name: str
    period_frames: int
    budget_instr: int
    runs: int
    overruns: int
    max_instr: int


@dataclass(frozen=True)
class Dump:
    """A parsed scheduler dump, read from guest RAM after the window closed."""

    window_frames: int
    frame_ticks: int
    frames_run: int
    trace_len: int
    trace_overflow: int
    frame_overruns: int
    slack_min: int
    window_start: int
    window_end: int
    tasks: tuple[Task, ...]
    trace: tuple[int, ...]


def parse_dump(text: str) -> Dump:
    """Parse the flat format emitted by ``harness/runner/dump_trace.gdb``."""
    scalars: dict[str, int] = {}
    tasks: list[Task] = []
    trace: dict[int, int] = {}
    complete = False

    for raw in text.splitlines():
        parts = raw.split()
        if not parts:
            continue
        if parts[0] == "DUMP-COMPLETE":
            complete = True
        elif parts[0] == "task" and len(parts) == 8:
            tasks.append(
                Task(
                    index=int(parts[1]),
                    name=parts[2].strip('"'),
                    period_frames=int(parts[3]),
                    budget_instr=int(parts[4]),
                    runs=int(parts[5]),
                    overruns=int(parts[6]),
                    max_instr=int(parts[7]),
                )
            )
        elif parts[0] == "trace" and len(parts) == 3:
            trace[int(parts[1])] = int(parts[2])
        elif len(parts) == 2 and parts[1].lstrip("-").isdigit():
            scalars[parts[0]] = int(parts[1])

    if not complete:
        raise ValueError(
            "the dump is truncated: DUMP-COMPLETE is absent, so the debugger "
            "did not finish reading guest memory and any verdict from it would "
            "be a verdict on a partial observation"
        )

    missing = {
        "window_frames",
        "frame_ticks",
        "frames_run",
        "trace_len",
        "trace_overflow",
        "frame_overruns",
        "slack_min",
        "window_start",
        "window_end",
    } - scalars.keys()
    if missing:
        raise ValueError(f"the dump is missing fields: {sorted(missing)}")

    ordered = tuple(trace[i] for i in sorted(trace))
    return Dump(
        window_frames=scalars["window_frames"],
        frame_ticks=scalars["frame_ticks"],
        frames_run=scalars["frames_run"],
        trace_len=scalars["trace_len"],
        trace_overflow=scalars["trace_overflow"],
        frame_overruns=scalars["frame_overruns"],
        slack_min=scalars["slack_min"],
        window_start=scalars["window_start"],
        window_end=scalars["window_end"],
        tasks=tuple(sorted(tasks, key=lambda t: t.index)),
        trace=ordered,
    )


def expected_trace(dump: Dump) -> tuple[int, ...]:
    """The dispatch sequence the table dictates, derived from the table itself.

    Table order within a frame, every task whose period divides the frame index.
    No tolerance and no alternative ordering: a cyclic executive with a static
    table has exactly one correct sequence.
    """
    sequence: list[int] = []
    for frame in range(dump.window_frames):
        for task in dump.tasks:
            if task.period_frames > 0 and frame % task.period_frames == 0:
                sequence.append(task.index)
    return tuple(sequence)


def check_conformance(dump: Dump) -> list[str]:
    """Execution counts must equal window / period exactly, with no tolerance."""
    failures: list[str] = []

    if dump.trace_overflow != 0:
        failures.append(
            f"trace overflowed by {dump.trace_overflow} dispatches, so the "
            "sequence compared below is truncated and proves nothing"
        )
    if dump.frames_run != dump.window_frames:
        failures.append(
            f"ran {dump.frames_run} frames, the window is {dump.window_frames}"
        )

    # The window must span exactly frames x frame_ticks. This is what catches an
    # executive that dispatches the right tasks in the right order but does not
    # actually wait out its frames: correct in everything the trace can see, and
    # wrong about time. Nothing else in this file would notice.
    span = (dump.window_end - dump.window_start) & 0xFFFFFFFF
    expected_span = dump.window_frames * dump.frame_ticks
    if span != expected_span:
        failures.append(
            f"the window spanned {span} ticks, the table dictates exactly "
            f"{expected_span} ({dump.window_frames} frames x {dump.frame_ticks} "
            "ticks) — the frames were not waited out"
        )

    for task in dump.tasks:
        expected = dump.window_frames // task.period_frames
        if task.runs != expected:
            failures.append(
                f"task {task.name!r} ran {task.runs} times, the table dictates "
                f"exactly {expected} ({dump.window_frames} frames / period "
                f"{task.period_frames})"
            )
    return failures


def check_order(dump: Dump) -> list[str]:
    """The trace must match the table's sequence position by position."""
    expected = expected_trace(dump)
    if dump.trace == expected:
        return []

    if len(dump.trace) != len(expected):
        return [
            f"trace has {len(dump.trace)} dispatches, the table dictates "
            f"{len(expected)}"
        ]
    for position, (got, want) in enumerate(zip(dump.trace, expected)):
        if got != want:
            return [
                f"trace diverges at dispatch {position}: got task {got}, "
                f"the table dictates task {want}"
            ]
    return []


def check_budgets(dump: Dump) -> list[str]:
    """Overruns are reported, never treated as a reason to stop.

    ADR 0003 fixes that M2 detects and counts overruns without preventing them.
    This function therefore reports what happened; whether an overrun is a
    failure depends on the campaign, and the caller decides.
    """
    notes: list[str] = []
    for task in dump.tasks:
        if task.overruns:
            notes.append(
                f"task {task.name!r} overran {task.overruns} times "
                f"(worst {task.max_instr} of {task.budget_instr} instructions)"
            )
    return notes


def compare_runs(first: Dump, second: Dump) -> list[str]:
    """Two runs of the same image must produce identical observations.

    Under ``-icount`` execution is deterministic, so any difference is a defect
    by definition: there is no non-determinism left to attribute it to. The
    instruction counts are compared as well as the dispatch sequence, because
    the sequence alone is driven by the tick clock and would stay identical even
    without ``-icount`` — it is the instruction figures that would move.
    """
    failures: list[str] = []

    if first.trace != second.trace:
        for position, (a, b) in enumerate(zip(first.trace, second.trace)):
            if a != b:
                failures.append(
                    f"dispatch sequences differ at position {position}: "
                    f"{a} then {b}"
                )
                break
        else:
            failures.append(
                f"dispatch sequences differ in length: {len(first.trace)} "
                f"then {len(second.trace)}"
            )

    for a, b in zip(first.tasks, second.tasks):
        if (a.runs, a.overruns, a.max_instr) != (b.runs, b.overruns, b.max_instr):
            failures.append(
                f"task {a.name!r} differs between runs: "
                f"runs {a.runs}/{b.runs}, overruns {a.overruns}/{b.overruns}, "
                f"max instructions {a.max_instr}/{b.max_instr}"
            )
    return failures


def main(argv: list[str]) -> int:
    """Check one dump, or compare two. Returns a process exit status."""
    if len(argv) not in (2, 3):
        print("usage: trace_check.py DUMP [DUMP2]", file=sys.stderr)
        return 2

    first = parse_dump(Path(argv[1]).read_text(encoding="utf-8"))
    failures = check_conformance(first) + check_order(first)

    if len(argv) == 3:
        second = parse_dump(Path(argv[2]).read_text(encoding="utf-8"))
        failures += compare_runs(first, second)

    for note in check_budgets(first):
        print(f"  note: {note}")

    if failures:
        print("FAIL")
        for failure in failures:
            print(f"  {failure}")
        return 1

    span = (first.window_end - first.window_start) & 0xFFFFFFFF
    print(
        f"  {first.frames_run} frames, {first.trace_len} dispatches, "
        f"{span} ticks spanned, order and counts match the table"
    )
    if len(argv) == 3:
        print("  two runs produced identical traces and instruction counts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
