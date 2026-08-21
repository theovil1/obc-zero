"""Assert what a stalled downlink proves, without measuring the instrumentation.

Three properties, from one run with the downlink held refusing.

**The cost is bracketed, from both sides, and neither side is a tolerance.**

Below: a refused dispatch must cost at least ``allowance * poll_instr`` more than
an accepted one. That is what proves the retries are spent rather than declared —
a build giving up on the first refusal shows no difference at all. It is a lower
bound and not an equality because the two paths do more than poll differently:
the refused one announces the drop and does not write forty-five bytes, and
predicting the net of those would be predicting the compiler.

Above: the executive's own overrun count must stay at zero. That is the budget
the task is judged against, applied by the thing that judges it, and it is the
bound that decides whether a congested downlink can climb the ladder.

Together they say the loop spends its allowance and stops.

**The system survives.** Exactly one boot banner. This is the assertion the first
version of this check did not make, and the omission mattered: the run it passed
had escalated to rung 3 and reset the machine over a congested downlink — the
precise outcome ADR 0009 forbids — and nothing looked.

**The shed frames are accounted for.** At least one surviving frame reports a
non-zero drop count, so a frame the vehicle chose not to send is distinguishable
from one lost on the link.

All three are read from the run rather than from the substitute's own behaviour.
The substitute selects its status source outside the poll loop, so the loop it
executes is instruction for instruction the flight build's; that is what makes an
absolute figure meaningful here at all.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from tlm_decode import find_frames, load_layout

# "  task telemetry runs=8 max=1504/3000 over=0"
_TASK_RE = re.compile(
    r"task\s+(?P<name>\S+)\s+runs=(?P<runs>\d+)\s+max=(?P<max>\d+)/(?P<budget>\d+)"
    r"\s+over=(?P<over>\d+)"
)


class CheckFailed(Exception):
    """A property the run was supposed to demonstrate did not hold."""


def telemetry_overruns(console: Path) -> int:
    """How many dispatches the executive judged over budget.

    Taken from the executive rather than recomputed, because the executive's
    verdict is the one that drives the escalation ladder. A second opinion formed
    on the host could disagree with the one that actually decided whether the
    machine reset.
    """
    text = console.read_text(errors="replace")
    for match in _TASK_RE.finditer(text):
        if match.group("name") == "telemetry":
            return int(match.group("over"))
    raise CheckFailed("the console carries no telemetry line")


def telemetry_cost(console: Path) -> int:
    """The telemetry task's worst dispatch, from the executive's own summary.

    Read from the console rather than through the debugger: the figure the
    scheduler reports is the one it judged the task against, and a second reading
    taken some other way could disagree with the one that drove the escalation.
    """
    text = console.read_text(errors="replace")
    for match in _TASK_RE.finditer(text):
        if match.group("name") == "telemetry":
            return int(match.group("max"))
    raise CheckFailed(
        "the console carries no telemetry line, so the run did not reach its "
        "summary and nothing about cost can be concluded"
    )


def _flight_figures(elf: Path) -> tuple[int, int, int, int]:
    """The allowance and the per-poll cost, read out of the binary under test.

    Never restated here. A prediction built from the harness's own copy of these
    numbers would keep agreeing with itself after the flight figures moved, which
    is the failure this whole arrangement exists to avoid.
    """
    out = subprocess.run(
        [
            "gdb-multiarch",
            "-batch",
            "-nx",
            "-ex",
            "set architecture riscv:rv32",
            "-ex",
            "print obc_uart_tx_retry_total",
            "-ex",
            "print obc_uart_tx_poll_instr",
            "-ex",
            "print obc_uart_tx_byte_instr",
            "-ex",
            "print obc_tlm_frame_len",
            str(elf),
        ],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    values = [
        int(line.split("=", 1)[1].strip())
        for line in out.splitlines()
        if line.startswith("$")
    ]
    if len(values) != 4:
        raise CheckFailed(
            "the binary does not carry its retry allowance, poll cost, byte cost "
            "and frame length as symbols, so the host would have to restate them"
        )
    return values[0], values[1], values[2], values[3]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--console", type=Path, required=True)
    parser.add_argument(
        "--expect-drops",
        action="store_true",
        help="require at least one frame to report a drop",
    )
    parser.add_argument(
        "--record-cost", type=Path, help="write this run's dispatch cost here"
    )
    parser.add_argument(
        "--nominal-cost",
        type=Path,
        help="the cost recorded by the same image with the port accepting",
    )
    parser.add_argument(
        "--one-boot",
        action="store_true",
        help="require the run to have booted exactly once",
    )
    parser.add_argument(
        "--poll-cost-from",
        type=Path,
        help="a cost recorded by the same image with a smaller allowance",
    )
    parser.add_argument(
        "--extra-polls",
        type=int,
        help="how many more polls this run's allowance permits",
    )
    parser.add_argument(
        "--no-overrun",
        action="store_true",
        help="require the executive to have judged no dispatch over budget",
    )
    args = parser.parse_args(argv)

    try:
        cost = telemetry_cost(args.console)

        if args.expect_drops:
            layout = load_layout(args.elf)
            frames = find_frames(args.capture.read_bytes(), layout)
            if not frames:
                raise CheckFailed(
                    "no frame survived, so the vehicle has no way to report that "
                    "it shed any — a downlink that drops everything must still be "
                    "distinguishable from one that was never asked to send"
                )
            dropped = max(int(f.values["frames_dropped"]) for f in frames)
            if dropped == 0:
                raise CheckFailed(
                    "the port refused past the allowance and no frame reports a "
                    "drop: the frames were shed silently, which from the ground is "
                    "indistinguishable from frames lost on the link"
                )
            print(f"    {len(frames)} frames survived, {dropped} reported shed", end="")

        if args.no_overrun:
            over = telemetry_overruns(args.console)
            if over != 0:
                raise CheckFailed(
                    f"the executive judged {over} telemetry dispatches over budget "
                    "while the downlink refused: the emission is not contained by "
                    "its allowance, so a congested link can still climb the ladder"
                )

        if args.one_boot:
            boots = args.console.read_text(errors="replace").count("=== OBC-Zero ===")
            if boots != 1:
                raise CheckFailed(
                    f"the run booted {boots} times: a congested downlink climbed the "
                    "ladder and reset the machine, which is the one outcome the "
                    "emission policy exists to prevent"
                )

        if args.nominal_cost:
            nominal = int(args.nominal_cost.read_text().strip())
            allowance, poll, byte_cost, frame_len = _flight_figures(args.elf)
            # A refused emission spends the allowance polling and saves the byte
            # writes it never performs. Netting the two is what makes this a
            # figure about the loop rather than about the frame length.
            floor = allowance * poll - frame_len * byte_cost
            observed = cost - nominal
            if observed < floor:
                raise CheckFailed(
                    f"a refused dispatch cost only {observed} instructions more than "
                    f"an accepted one, against {floor} predicted "
                    f"({allowance} polls x {poll}, less {frame_len} bytes x "
                    f"{byte_cost} the refused path never writes): "
                    "the retries are declared "
                    "and not spent, so the port is being given up on rather than "
                    "waited out"
                )
            print(
                f", {observed} instructions spent waiting, at least {floor} of them polling"
            )

        if args.poll_cost_from and args.extra_polls:
            before = int(args.poll_cost_from.read_text().strip())
            _allowance, poll, _byte, _len = _flight_figures(args.elf)
            diff = cost - before
            # Bracketed rather than divided, and not as a tolerance: the two runs
            # differ by the polls *and* by a fixed instruction or two of loop
            # setup, so the difference is `extra x poll` plus a residual smaller
            # than one poll. Requiring the residual to stay under one poll pins
            # the per-poll figure to an exact integer — poll-1 and poll+1 both
            # fail — without inventing a margin.
            if not (args.extra_polls * poll <= diff < args.extra_polls * (poll + 1)):
                raise CheckFailed(
                    f"{args.extra_polls} extra polls cost {diff} instructions, "
                    f"which does not put one poll at {poll} "
                    f"(expected {args.extra_polls * poll} to "
                    f"{args.extra_polls * (poll + 1) - 1}). A measured constant is "
                    "only a control while something re-measures it — and this one "
                    "feeds the assertion that keeps a congested downlink from "
                    "resetting the machine"
                )
            residual = diff - args.extra_polls * poll
            print(
                f", one poll costs {poll} instructions as the build asserts "
                f"({diff} for {args.extra_polls}, residual {residual})"
            )

        if args.record_cost:
            args.record_cost.write_text(f"{cost}\n")

    except CheckFailed as failure:
        print(f"\nSTALL-CHECK FAIL: {failure}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
