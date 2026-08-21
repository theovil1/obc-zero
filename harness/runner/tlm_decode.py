"""Decode housekeeping telemetry, using the layout the flight software carries.

The acceptance criterion for M6 is that the flight code and this decoder take
their field definitions from one source of truth. The source of truth is
``obc_tlm_fields[]`` in ``flight/tlm/frame.c``, and this module reads it out of
the ELF under test rather than restating it.

That is the same reasoning as M2's conformance checker, which derives its
expected dispatch counts from the task table in the binary. A restated layout
would not fail loudly when it drifted: it would decode a frame into plausible
wrong numbers, and a campaign would publish them as measurements.

**What is duplicated, deliberately, is the integrity sum.** An independent
implementation is the only way for the host to check the vehicle's arithmetic;
reading the algorithm out of the binary would mean verifying a frame against the
same code that produced it, which verifies nothing. The criterion is about field
definitions, and that distinction is the reason it can be met while this exists.
"""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path

GDB = "gdb-multiarch"

# Field kinds, mirrored from flight/tlm/frame.h. These are names for values the
# binary already carries; the offsets and widths — the things that decide how
# bytes are read — are never restated here.
KIND_SYNC = 0
KIND_COUNT = 1
KIND_ENUM = 2
KIND_BITS = 3
KIND_SENSOR = 4
KIND_SUM = 5

SENSOR_INVALID = 0xFFFF


@dataclass(frozen=True)
class Field:
    """One field of the frame, as the flight software declares it."""

    name: str
    offset: int
    width: int
    count: int
    kind: int


@dataclass(frozen=True)
class Layout:
    """The frame layout, read from a binary."""

    fields: tuple[Field, ...]
    frame_len: int
    sync: int
    sensor_ranges: tuple[tuple[str, int, int], ...]
    stuck_limit: int

    def field(self, name: str) -> Field:
        for f in self.fields:
            if f.name == name:
                return f
        raise KeyError(f"no field named {name!r} in this binary's layout")


@dataclass(frozen=True)
class Frame:
    """A decoded frame, plus where it was found in the stream."""

    offset: int
    values: dict[str, int | list[int]]

    def flagged_sensors(self) -> list[int]:
        """Indices whose reading is not trustworthy, per the frame's own flags."""
        flags = int(self.values["sensor_flags"])
        readings = self.values["sensor"]
        assert isinstance(readings, list)
        return [i for i in range(len(readings)) if (flags >> (3 * i)) & 0x7]


def _gdb(elf: Path, exprs: list[str]) -> list[str]:
    """Print expressions from a static ELF. No target, no running QEMU.

    ``set architecture`` comes before anything else. gdb-multiarch reports itself
    as configured for the host — that string names its host, not its targets —
    and letting it guess is how a RISC-V binary gets read as x86.
    """
    cmd = [GDB, "-batch", "-nx", "-ex", "set architecture riscv:rv32"]
    for e in exprs:
        cmd += ["-ex", f"print {e}"]
    cmd.append(str(elf))
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    return [
        line.split("=", 1)[1].strip()
        for line in out.splitlines()
        if line.startswith("$")
    ]


def _scalar(text: str) -> int:
    """Read one GDB scalar.

    A ``char`` field prints as ``4 '\\004'`` — the numeric value followed by the
    same value as a character literal. Everything from the first quote onwards is
    that redundant rendering. ``base=0`` keeps hexadecimal working, which the sync
    word needs.
    """
    return int(text.split("'", 1)[0].strip(), 0)


def _members(record: str) -> dict[str, str]:
    """Split one ``{a = 1, b = 2}`` record, ignoring commas inside quotes."""
    out: dict[str, str] = {}
    depth = 0
    in_str = False
    escaped = False
    part = ""
    parts: list[str] = []
    for ch in record.strip().removeprefix("{").removesuffix("}"):
        if escaped:
            escaped = False
        elif ch == "\\":
            escaped = True
        elif ch == '"':
            in_str = not in_str
        elif not in_str and ch in "{[":
            depth += 1
        elif not in_str and ch in "}]":
            depth -= 1
        elif not in_str and depth == 0 and ch == ",":
            parts.append(part)
            part = ""
            continue
        part += ch
    parts.append(part)
    for p in parts:
        if "=" in p:
            k, v = p.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def _records(array: str) -> list[str]:
    """Split ``{{...}, {...}}`` into its top-level records."""
    body = array.strip().removeprefix("{").removesuffix("}")
    out: list[str] = []
    depth = 0
    in_str = False
    escaped = False
    cur = ""
    for ch in body:
        if escaped:
            escaped = False
        elif ch == "\\":
            escaped = True
        elif ch == '"':
            in_str = not in_str
        elif not in_str and ch == "{":
            depth += 1
        elif not in_str and ch == "}":
            depth -= 1
            if depth == 0:
                out.append(cur + ch)
                cur = ""
                continue
        if depth > 0:
            cur += ch
    return out


def load_layout(elf: Path) -> Layout:
    """Read the frame layout and the sensor ranges out of a binary.

    Both come from the same place for the same reason: a plausibility bound
    restated on the host would drift away from the one the vehicle applies, and
    the host would then be checking its own opinion.
    """
    raw = _gdb(
        elf,
        [
            "obc_tlm_frame_len",
            "obc_tlm_sync",
            "obc_tlm_fields",
            "obc_sensor_desc",
            "obc_sensor_stuck_limit",
        ],
    )
    frame_len = _scalar(raw[0])
    sync = _scalar(raw[1])

    fields: list[Field] = []
    for rec in _records(raw[2]):
        m = _members(rec)
        fields.append(
            Field(
                name=m["name"].split('"')[1],
                offset=_scalar(m["offset"]),
                width=_scalar(m["width"]),
                count=_scalar(m["count"]),
                kind=_scalar(m["kind"]),
            )
        )

    ranges: list[tuple[str, int, int]] = []
    for rec in _records(raw[3]):
        m = _members(rec)
        ranges.append((m["name"].split('"')[1], _scalar(m["min"]), _scalar(m["max"])))

    return Layout(
        fields=tuple(fields),
        frame_len=frame_len,
        sync=sync,
        sensor_ranges=tuple(ranges),
        stuck_limit=_scalar(raw[4]),
    )


def frame_sum(data: bytes, upto: int) -> int:
    """The vehicle's integrity sum, reimplemented.

    Deliberately a second implementation — see the module docstring. It is the
    only part of this file that would go wrong silently if the flight side
    changed, and it is the only part that has to.
    """
    total = 0
    for byte in data[:upto]:
        total += byte
        total = (total + (total >> 16)) & 0xFFFF
    return total ^ 0xFFFF


def decode_frame(raw: bytes, layout: Layout) -> dict[str, int | list[int]]:
    """Turn ``layout.frame_len`` bytes into named values."""
    values: dict[str, int | list[int]] = {}
    for f in layout.fields:
        elems = [
            int.from_bytes(
                raw[f.offset + i * f.width : f.offset + (i + 1) * f.width], "little"
            )
            for i in range(f.count)
        ]
        values[f.name] = elems[0] if f.count == 1 else elems
    return values


def find_frames(stream: bytes, layout: Layout) -> list[Frame]:
    """Recover every frame from a capture that also carries plain text.

    A sync word is treated as a *candidate*, never as a boundary. The payload can
    and eventually will contain the sync pattern — a counter passing through it
    is enough — so a decoder that consumed a whole frame on every match would
    step over real frames and report a clean run. On a failed sum this advances
    by one byte and looks again.
    """
    sync = layout.sync.to_bytes(4, "little")
    sum_field = next(f for f in layout.fields if f.kind == KIND_SUM)
    frames: list[Frame] = []
    i = 0
    while i + layout.frame_len <= len(stream):
        at = stream.find(sync, i)
        if at < 0 or at + layout.frame_len > len(stream):
            break
        raw = stream[at : at + layout.frame_len]
        want = int.from_bytes(
            raw[sum_field.offset : sum_field.offset + sum_field.width], "little"
        )
        if frame_sum(raw, sum_field.offset) == want:
            frames.append(Frame(offset=at, values=decode_frame(raw, layout)))
            i = at + layout.frame_len
        else:
            i = at + 1
    return frames
