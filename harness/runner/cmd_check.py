"""Drive the uplink and assert what the command path did — and did not do.

Two kinds of assertion, and ADR 0011 says why they are different kinds.

**Robustness.** A malformed frame must be refused and change nothing else. That
is not fault tolerance: nothing failed, something arrived. Success is the vehicle
being as it was, plus one rejection record.

**Coverage.** ADR 0013: a fuzzer that never reaches the parser produces exactly
the result of a perfect parser, so the campaign has to prove it did something.
The identity below is the part that cannot be satisfied by doing nothing:

    examined == accepted + sum(rejected)

A frame in neither column was silently discarded, and silent discard is what a
passing campaign looks like from outside.

Every bound and offset used here is read from the binary by ``uplink``. The
boundary arguments in particular are *derived* from the flight table rather than
retyped, so an off-by-one in the flight comparison cannot move the test's idea of
the boundary with it.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from tlm_decode import Frame, find_frames, load_layout
from uplink import Uplink, UplinkLayout, boundary_arguments, build, load_uplink

REASON_NAMES = (
    "sync",
    "length",
    "sum",
    "opcode",
    "arg",
    "replay",
    "queue",
    "not-armed",
)


class CheckFailed(Exception):
    """A property the run was supposed to demonstrate did not hold."""


# How many frames one run can actually ingest.
#
# **Not the baud rate.** The uplink's throughput is set by the receive FIFO and
# the poll period: the vehicle drains the FIFO once per dispatch, and QEMU refills
# it when the main loop next runs, so about fifteen bytes reach the parser per
# frame. Over the executive's sixteen-frame window that is roughly twelve frames,
# measured — a first version sent thirty-one and reported the checks it never
# reached as never firing.
#
# This is a property of the link and not of the emulator. A ground station that
# transmits faster than the vehicle polls does not get its commands in, on this
# machine or on any other, and the answer is pacing rather than a bigger buffer.
FRAMES_PER_RUN = 12


def send_every_reason(link: Uplink, up: UplinkLayout, phase: str) -> dict[str, int]:
    """Send frames chosen to reach a phase's rejection reasons.

    Hand-written rather than fuzzed, because a random generator will not
    reliably produce a frame that is valid in every respect except its arming.
    The fuzz campaign is the other half; neither replaces the other.

    Split into phases because one run cannot carry every case — see
    FRAMES_PER_RUN. Each phase is its own boot, which also means each phase
    asserts independently that the vehicle survived it.

    Returns the expected count per reason, so the assertions compare against a
    prediction rather than against whatever happened.
    """
    ping = up.command("ping")
    setter = up.command("set")
    safe = up.command("safe")
    arm = up.command("arm")
    expect: dict[str, int] = dict.fromkeys(REASON_NAMES, 0)
    counter = 1

    def send(**kw: object) -> None:
        """Queue a frame into the stream, without pausing.

        No wait between frames, and the reason is measured rather than
        stylistic: the executive's window is sixteen frames, and pausing 50 ms
        of wall clock per frame closed it before half of these arrived. The
        first version did exactly that and reported the sync check as never
        firing — a red naming the vehicle for a defect in the harness.

        Order is preserved by the stream, which is what the replay and arming
        cases depend on.
        """
        nonlocal counter
        link.send(build(up, counter=counter, **kw))  # type: ignore[arg-type]
        counter += 1

    # Accepted first in every phase, so the accept path is exercised before
    # anything else and a later failure cannot be blamed on a parser that never
    # accepts anything at all.
    send(opcode=ping.opcode)

    if phase == "parse":
        # The four checks a frame meets before its contents matter, and the
        # boundary arguments derived from the flight table's own bounds.
        send(opcode=ping.opcode, sync=up.sync ^ 0xFFFFFFFF)
        expect["sync"] += 1
        send(opcode=ping.opcode, length=up.frame_len + 1)
        expect["length"] += 1
        send(opcode=ping.opcode, corrupt_sum=True)
        expect["sum"] += 1
        send(opcode=0xEE)
        expect["opcode"] += 1

        for _label, arg, accepted in boundary_arguments(setter):
            send(opcode=setter.opcode, arg=arg)
            if not accepted:
                expect["arg"] += 1

    elif phase == "sequence":
        # A replay: the counter does not advance past the last accepted one.
        link.send(build(up, opcode=ping.opcode, counter=1))
        expect["replay"] += 1

        # Critical without arming, then armed and accepted. Both directions: the
        # refusal proves the gate exists, the acceptance proves it is not simply
        # closed — a gate that refuses everything passes the first half.
        send(opcode=safe.opcode)
        expect["not-armed"] += 1
        send(opcode=arm.opcode, arg=safe.opcode)
        send(opcode=safe.opcode)

    elif phase == "queue":
        # More time-tagged commands than the queue holds, all far enough in the
        # future that none drains before the next arrives.
        # One more than the queue holds, read from the binary rather than
        # restated: a test that knew the depth by heart would keep passing after
        # the depth changed, and would be filling something else.
        for _ in range(up.queue_len + 1):
            send(opcode=ping.opcode, when=0xFFFFFF00)
        expect["queue"] += 1

    else:
        raise CheckFailed(f"no such phase: {phase}")

    return expect


def last_frame(frames: list[Frame]) -> Frame:
    if not frames:
        raise CheckFailed(
            "no telemetry frame survived, so the vehicle's counters were never "
            "read and nothing about the command path can be concluded"
        )
    return frames[-1]


def check(elf: Path, downlink: bytes, expect: dict[str, int], console: Path) -> None:
    """Assert coverage, the identity, and that the vehicle did not move."""
    layout = load_layout(elf)
    frames = find_frames(downlink, layout)
    values = last_frame(frames).values

    examined = int(values["cmd_examined"])
    accepted = int(values["cmd_accepted"])
    rejected = values["cmd_rejected"]
    assert isinstance(rejected, list)

    # ADR 0013 obligation 2, and the one assertion doing nothing cannot satisfy.
    if examined != accepted + sum(rejected):
        raise CheckFailed(
            f"examined {examined} but accepted {accepted} plus rejected "
            f"{sum(rejected)} is {accepted + sum(rejected)}: "
            f"{examined - accepted - sum(rejected)} frames were neither, which "
            "means they were silently discarded"
        )

    if accepted == 0:
        raise CheckFailed(
            "no frame was accepted, so the accept path is untested and "
            "'nothing happened' is true because nothing was asked to happen"
        )
    if int(values["cmd_executed"]) == 0:
        raise CheckFailed("frames were accepted and none executed")

    for index, name in enumerate(REASON_NAMES):
        if expect[name] > 0 and rejected[index] < expect[name]:
            raise CheckFailed(
                f"reason '{name}' was reached {rejected[index]} times against "
                f"{expect[name]} expected: the check that produces it did not fire"
            )
        if expect[name] == 0 and rejected[index] != 0:
            raise CheckFailed(
                f"reason '{name}' fired {rejected[index]} times and nothing sent "
                "should have produced it"
            )

    # Robustness, in ADR 0011's sense: the refusals must not have moved the
    # vehicle. A command path that reached the escalation ladder would show here.
    if int(values["mode"]) != 0:
        raise CheckFailed(
            "the vehicle is degraded after a run of malformed frames: an input "
            "reached the recovery path, so anyone who can transmit can degrade it"
        )
    if int(values["suspensions"]) != 0:
        raise CheckFailed("a malformed frame caused a task suspension")

    text = console.read_text(errors="replace")
    if text.count("=== OBC-Zero ===") != 1:
        raise CheckFailed(
            "the vehicle rebooted during a run of malformed frames: garbage on "
            "the uplink reached the reset rung"
        )

    reached = [REASON_NAMES[i] for i, n in enumerate(rejected) if n > 0]
    print(
        f"{examined} examined = {accepted} accepted + {sum(rejected)} rejected, "
        f"{int(values['cmd_executed'])} executed, "
        f"reasons: {', '.join(reached) if reached else 'none'}, "
        "mode nominal, one boot"
    )


def _dump(elf: Path, downlink: bytes) -> None:
    """What the vehicle actually reported, printed on failure.

    A failure that says only which assertion broke leaves the reader to rerun it
    to find out what happened. The counters are already in the frame.
    """
    try:
        layout = load_layout(elf)
        frames = find_frames(downlink, layout)
        if not frames:
            print("    no telemetry frame decoded", file=sys.stderr)
            return
        values = frames[-1].values
        rejected = values["cmd_rejected"]
        assert isinstance(rejected, list)
        print(
            f"    examined={values['cmd_examined']} accepted={values['cmd_accepted']} "
            f"executed={values['cmd_executed']} queued={values['cmd_queued']}",
            file=sys.stderr,
        )
        for index, name in enumerate(REASON_NAMES):
            print(f"    {name:<10} {rejected[index]}", file=sys.stderr)
    except Exception as err:  # noqa: BLE001 - diagnosis must not mask the failure
        print(f"    (could not decode the downlink: {err})", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--console", type=Path, required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--settle", type=float, default=3.0)
    parser.add_argument(
        "--phase", choices=("parse", "sequence", "queue"), required=True
    )
    args = parser.parse_args(argv)

    up = load_uplink(args.elf)
    link = Uplink(args.port)
    try:
        expect = send_every_reason(link, up, args.phase)
        downlink = link.drain(args.settle)
    finally:
        link.close()

    try:
        check(args.elf, downlink, expect, args.console)
    except CheckFailed as failure:
        print(f"CMD-CHECK FAIL: {failure}", file=sys.stderr)
        _dump(args.elf, downlink)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
