"""Assert what a telemetry capture proves, in both directions.

The acceptance criterion says a sensor returning garbage must be flagged and not
propagated. That sentence contains two claims, and only the first one is ever
remembered:

    flagged     a rejected reading never appears as a value
    real        an unflagged reading is one the sensor actually produced

A checker that tests only the first passes on an implementation that flags
everything — including one that flags a perfectly good sensor for the whole
mission. This project has already paid for the missing direction once, in M5's
rung-1 suspension, where "every gap has an announcement" was asserted and "every
announcement has a gap" was not, and the suspension suspended nothing.

Every bound this compares against is read from the binary under test. Nothing
here restates a range, a threshold, or an offset.
"""

from __future__ import annotations

import argparse
import sys
from itertools import pairwise
from pathlib import Path

from tlm_decode import SENSOR_INVALID, Frame, Layout, find_frames, load_layout

FLAG_BITS = 3
BIT_RANGE = 0
BIT_STUCK = 1
BIT_NODATA = 2


class CheckFailed(Exception):
    """A property the capture was supposed to demonstrate did not hold."""


def _flag(frame: Frame, sensor: int, bit: int) -> bool:
    return bool((int(frame.values["sensor_flags"]) >> (sensor * FLAG_BITS + bit)) & 1)


def _reading(frame: Frame, sensor: int) -> int:
    readings = frame.values["sensor"]
    assert isinstance(readings, list)
    return readings[sensor]


def check_frames_present(frames: list[Frame], least: int) -> None:
    """A capture with no frames proves nothing, and must not read as a pass.

    The failure this guards against is a run that crashed before telemetry ever
    dispatched: every per-frame assertion below is vacuously true over an empty
    list, so without this the most broken possible run is the quietest.
    """
    if len(frames) < least:
        raise CheckFailed(f"{len(frames)} frames decoded, expected at least {least}")


def check_sequence(frames: list[Frame]) -> int:
    """Sequence numbers advance by one, except where a rung-2 reset returns them
    to zero.

    Returns the number of resets observed. ADR 0008 chose to break monotonicity
    deliberately so that a subsystem reset is visible in the frame itself; this
    is the assertion that the break is a reset and not a lost frame.
    """
    resets = 0
    for previous, current in pairwise(frames):
        before = int(previous.values["seq"])
        after = int(current.values["seq"])
        if after == before + 1:
            continue
        if after == 0:
            resets += 1
            continue
        raise CheckFailed(
            f"sequence jumped {before} -> {after} at byte {current.offset}: "
            "neither the next frame nor a subsystem reset, so a frame was lost"
        )
    return resets


def check_both_directions(frames: list[Frame], layout: Layout) -> None:
    """The two halves of "flagged, not propagated", over every frame."""
    for frame in frames:
        for sensor, (name, low, high) in enumerate(layout.sensor_ranges):
            value = _reading(frame, sensor)
            flagged = any(
                _flag(frame, sensor, b) for b in (BIT_RANGE, BIT_STUCK, BIT_NODATA)
            )

            if flagged and value != SENSOR_INVALID:
                raise CheckFailed(
                    f"frame at {frame.offset}: sensor {name} is flagged but carries "
                    f"{value}, so a rejected reading was published anyway"
                )
            if not flagged and not low <= value <= high:
                raise CheckFailed(
                    f"frame at {frame.offset}: sensor {name} carries {value}, outside "
                    f"[{low},{high}], with no flag set — the flag is decorative"
                )


def check_honest(frames: list[Frame], layout: Layout, sensor: int) -> None:
    """A sensor fed plausible varying values must end up unflagged.

    This is the case that fails on a validator which flags everything, and it is
    the only reason the other cases mean anything.
    """
    clean = [
        f
        for f in frames
        if not any(_flag(f, sensor, b) for b in (BIT_RANGE, BIT_STUCK, BIT_NODATA))
    ]
    if not clean:
        raise CheckFailed(
            f"sensor {layout.sensor_ranges[sensor][0]} was fed values inside its own "
            "declared range and was flagged in every frame: the validator rejects "
            "good readings"
        )
    low, high = layout.sensor_ranges[sensor][1:]
    for frame in clean:
        value = _reading(frame, sensor)
        if not low <= value <= high:
            raise CheckFailed(
                f"frame at {frame.offset}: unflagged reading {value} is outside "
                f"[{low},{high}]"
            )


def check_out_of_range(frames: list[Frame], layout: Layout, sensor: int) -> None:
    """Out-of-range readings are caught, by the range detector and not by luck."""
    name = layout.sensor_ranges[sensor][0]
    caught = [f for f in frames if _flag(f, sensor, BIT_RANGE)]
    if not caught:
        raise CheckFailed(
            f"sensor {name} was fed values outside its range and never flagged"
        )
    for frame in caught:
        if _flag(frame, sensor, BIT_STUCK):
            raise CheckFailed(
                f"frame at {frame.offset}: sensor {name} is flagged stuck as well as "
                "out of range, but the injected values varied — the case tests two "
                "detectors at once and proves neither"
            )
        if _reading(frame, sensor) != SENSOR_INVALID:
            raise CheckFailed(
                f"frame at {frame.offset}: out-of-range reading was published as "
                f"{_reading(frame, sensor)}"
            )


def check_stuck(frames: list[Frame], layout: Layout, sensor: int) -> None:
    """A plausible value that never changes is caught, and not before it should be.

    Both edges matter. A detector that flags on the first repeat would catch this
    too, and would also flag every real sensor whose reading happened to repeat
    once. The threshold is read from the binary so that this assertion follows it
    when it changes.
    """
    name = layout.sensor_ranges[sensor][0]
    stuck_from = None
    for index, frame in enumerate(frames):
        if _flag(frame, sensor, BIT_STUCK):
            stuck_from = index
            break

    if stuck_from is None:
        raise CheckFailed(
            f"sensor {name} was held at one plausible value for {len(frames)} frames "
            f"and never flagged stuck (threshold {layout.stuck_limit} samples)"
        )

    for frame in frames[stuck_from:]:
        if _reading(frame, sensor) != SENSOR_INVALID:
            raise CheckFailed(
                f"frame at {frame.offset}: a stuck reading was published as "
                f"{_reading(frame, sensor)}"
            )

    if _flag(frames[0], sensor, BIT_STUCK):
        raise CheckFailed(
            f"sensor {name} was flagged stuck in the very first frame, before "
            f"{layout.stuck_limit} samples could have been taken: the detector fires "
            "on repetition it has not yet seen"
        )


CHECKS = {"honest": check_honest, "range": check_out_of_range, "stuck": check_stuck}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True, help="binary under test")
    parser.add_argument(
        "--capture", type=Path, required=True, help="raw serial capture"
    )
    parser.add_argument(
        "--expect", choices=sorted(CHECKS), help="the injected case, if any"
    )
    parser.add_argument(
        "--sensor", type=int, default=0, help="which sensor was injected"
    )
    parser.add_argument(
        "--least-frames", type=int, default=4, help="minimum frames to accept"
    )
    args = parser.parse_args(argv)

    layout = load_layout(args.elf)
    frames = find_frames(args.capture.read_bytes(), layout)

    try:
        check_frames_present(frames, args.least_frames)
        resets = check_sequence(frames)
        check_both_directions(frames, layout)
        if args.expect:
            CHECKS[args.expect](frames, layout, args.sensor)
    except CheckFailed as failure:
        print(f"TLM-CHECK FAIL: {failure}", file=sys.stderr)
        return 1

    case = args.expect or "nominal"
    print(
        f"TLM-CHECK OK: {len(frames)} frames, {layout.frame_len}-byte layout read from "
        f"{args.elf}, case={case}, subsystem resets={resets}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
