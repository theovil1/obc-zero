"""Build command frames from the layout the vehicle carries, and drive an uplink.

Every offset, width, opcode and argument bound here is read out of the ELF under
test. Nothing is restated.

That discipline matters more on the uplink than it did on the downlink. A decoder
holding a stale layout misreads a report; **a fuzzer holding a stale layout is
testing a format the vehicle does not speak**, and would report a hundred
thousand clean rejections as evidence that the parser works.

The same applies to the boundary cases. An argument bound copied into this file
would move when somebody edited it here and not when the flight table changed —
so an off-by-one in flight code would produce a boundary case off by the same
one, and it would pass. `boundary_arguments()` derives its values from the bounds
in the binary for exactly that reason.
"""

from __future__ import annotations

import socket
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

GDB = "gdb-multiarch"


@dataclass(frozen=True)
class Command:
    """One row of the flight command table."""

    name: str
    opcode: int
    critical: bool
    arg_min: int
    arg_max: int


@dataclass(frozen=True)
class UplinkLayout:
    """The command frame's shape, as the vehicle declares it."""

    frame_len: int
    sync: int
    offsets: dict[str, tuple[int, int]]
    commands: tuple[Command, ...]
    reject_count: int
    queue_len: int

    def command(self, name: str) -> Command:
        for c in self.commands:
            if c.name == name:
                return c
        raise KeyError(f"no command named {name!r} in this binary's table")


def _gdb(elf: Path, exprs: list[str]) -> list[str]:
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
    return int(text.split("'", 1)[0].strip(), 0)


def _records(array: str) -> list[str]:
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


def _members(record: str) -> dict[str, str]:
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


def load_uplink(elf: Path) -> UplinkLayout:
    """Read the frame layout and the command table out of a binary."""
    count = _scalar(_gdb(elf, ["obc_cmd_count"])[0])
    raw = _gdb(
        elf,
        [
            "obc_cmd_frame_len",
            "obc_cmd_sync",
            "obc_cmd_reject_count",
            "obc_cmd_queue_len",
            "obc_cmd_fields",
            f"*obc_cmd_table@{count}",
        ],
    )

    offsets: dict[str, tuple[int, int]] = {}
    for rec in _records(raw[4]):
        m = _members(rec)
        offsets[m["name"].split('"')[1]] = (_scalar(m["offset"]), _scalar(m["width"]))

    commands: list[Command] = []
    for rec in _records(raw[5]):
        m = _members(rec)
        commands.append(
            Command(
                name=m["name"].split('"')[1],
                opcode=_scalar(m["opcode"]),
                critical=_scalar(m["critical"]) != 0,
                arg_min=_scalar(m["arg_min"]),
                arg_max=_scalar(m["arg_max"]),
            )
        )

    return UplinkLayout(
        frame_len=_scalar(raw[0]),
        sync=_scalar(raw[1]),
        reject_count=_scalar(raw[2]),
        queue_len=_scalar(raw[3]),
        offsets=offsets,
        commands=tuple(commands),
    )


def frame_sum(data: bytes, upto: int) -> int:
    """The vehicle's integrity word, reimplemented.

    Duplicated deliberately, as on the downlink: computing it with the vehicle's
    own code would check a frame against the thing that produced it. The
    *criterion* is about field definitions, and those are read from the binary.
    """
    total = 0
    for byte in data[:upto]:
        total += byte
        total = (total + (total >> 16)) & 0xFFFF
    return total ^ 0xFFFF


def build(
    layout: UplinkLayout,
    *,
    opcode: int,
    counter: int,
    arg: int = 0,
    when: int = 0,
    length: int | None = None,
    sync: int | None = None,
    corrupt_sum: bool = False,
) -> bytes:
    """Assemble one frame at the offsets the vehicle declares.

    Every keyword that can be wrong is a keyword on purpose: a test that can only
    build valid frames cannot exercise a rejection, and one that hand-assembles
    invalid frames from its own idea of the layout is testing its own idea.
    """
    buf = bytearray(layout.frame_len)

    def put(field: str, value: int) -> None:
        offset, width = layout.offsets[field]
        buf[offset : offset + width] = int(value).to_bytes(width, "little")

    put("sync", layout.sync if sync is None else sync)
    put("counter", counter)
    put("length", layout.frame_len if length is None else length)
    put("opcode", opcode)
    put("when", when)
    put("arg", arg)

    sum_offset, sum_width = layout.offsets["sum"]
    value = frame_sum(bytes(buf), sum_offset)
    if corrupt_sum:
        value ^= 0x0001
    buf[sum_offset : sum_offset + sum_width] = value.to_bytes(sum_width, "little")
    return bytes(buf)


def boundary_arguments(cmd: Command) -> list[tuple[str, int, bool]]:
    """The values either side of a command's declared bounds.

    Derived from the bounds in the binary, never retyped. A fuzzer will not
    reliably find the value one past a limit, so boundaries are written by hand —
    and a hand-written bound copied from the source is the second list this whole
    discipline exists to forbid. An off-by-one in the flight comparison would
    otherwise move the flight bound and the test's bound together, and the case
    would pass.

    Returns (label, argument, should_be_accepted).
    """
    cases: list[tuple[str, int, bool]] = [
        ("min", cmd.arg_min, True),
        ("max", cmd.arg_max, True),
    ]
    if cmd.arg_min > 0:
        cases.append(("min-1", cmd.arg_min - 1, False))
    if cmd.arg_max < 0xFFFFFFFF:
        cases.append(("max+1", cmd.arg_max + 1, False))
    if cmd.arg_max > cmd.arg_min:
        cases.append(("mid", cmd.arg_min + (cmd.arg_max - cmd.arg_min) // 2, True))
    return cases


class Uplink:
    """A live link to a running vehicle, over the socket QEMU exposes."""

    def __init__(self, port: int, timeout: float = 10.0) -> None:
        deadline = time.monotonic() + timeout
        last: OSError | None = None
        while time.monotonic() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError as err:  # QEMU has not opened the port yet
                last = err
                time.sleep(0.05)
        else:
            raise RuntimeError(f"no uplink on port {port}: {last}")
        self.sock.settimeout(0.2)
        self.received = bytearray()

    def send(self, frame: bytes) -> None:
        self.sock.sendall(frame)

    def drain(self, seconds: float) -> bytes:
        """Read whatever the downlink offers for a while.

        The vehicle transmits on the same socket, because the uplink and the
        downlink are one ground link — ADR 0009 decision 6. Reading has to
        continue while sending, or the socket fills and the vehicle's writes
        block against a peer that is not listening.
        """
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                self.received += chunk
            except TimeoutError:
                pass
        return bytes(self.received)

    def close(self) -> None:
        self.sock.close()
