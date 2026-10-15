"""Echo benchmark: send frames to an echoing peer and verify every one.

Each payload starts with a 32-bit frame counter and ends with a CRC-32 of the
rest, both computed here, outside framelink. A frame that framelink delivered
but that fails this check is counted as `undetected`: corruption that got
past both of framelink's CRCs. See docs/MEASUREMENTS.md, "Common rules".

SPDX-License-Identifier: MIT
Copyright (c) 2026 Guilherme Schwonka
"""
from __future__ import annotations

import time
import zlib
from dataclasses import asdict, dataclass, field
from typing import Dict, Optional

from .link import Busy, Config, Event, Link

MIN_SIZE = 8


def make_payload(index: int, size: int) -> bytes:
    if size < MIN_SIZE:
        raise ValueError(f"payload size must be at least {MIN_SIZE}")
    body = index.to_bytes(4, "little") + bytes((index * 131 + k) & 0xFF for k in range(size - 8))
    return body + zlib.crc32(body).to_bytes(4, "little")


def check_payload(payload: bytes):
    """Return the frame index, or None if the end-to-end CRC-32 fails."""
    if len(payload) < MIN_SIZE:
        return None
    body, crc = payload[:-4], payload[-4:]
    if zlib.crc32(body).to_bytes(4, "little") != crc:
        return None
    return int.from_bytes(body[:4], "little")


@dataclass
class BenchResult:
    count: int
    size: int
    corrupt_rate: float
    delivered: int = 0
    undetected: int = 0
    out_of_order: int = 0
    elapsed_s: float = 0.0
    goodput_bps: float = 0.0
    link_down_events: int = 0
    busy_refusals: int = 0
    completed: bool = False
    stats: Dict[str, int] = field(default_factory=dict)

    def as_dict(self) -> dict:
        return asdict(self)


def run_echo_bench(
    transport,
    count: int,
    size: int = 16,
    config: Optional[Config] = None,
    timeout_s: float = 120.0,
    corrupt_rate: float = 0.0,
) -> BenchResult:
    """Send `count` frames through `transport` to an echo peer such as fl_peer."""
    result = BenchResult(count=count, size=size, corrupt_rate=corrupt_rate)
    events = {"down": 0}

    def on_event(ev: Event, arg: int) -> None:
        if ev is Event.LINK_DOWN:
            events["down"] += 1

    link = Link(transport.write, config or Config(), on_event)
    start = time.monotonic()
    deadline = start + timeout_s
    sent = 0
    expected = 0

    def now_ms() -> int:
        return int((time.monotonic() - start) * 1000)

    while expected < count and time.monotonic() < deadline:
        link.poll(now_ms())
        try:
            data = transport.read(0.001)
        except ConnectionError:
            break
        if data:
            link.input(data)

        while link.connected and sent < count:
            try:
                link.send(0x01, make_payload(sent, size))
            except Busy:
                result.busy_refusals += 1
                break
            sent += 1

        while True:
            frame = link.recv()
            if frame is None:
                break
            index = check_payload(frame.payload)
            if index is None:
                result.undetected += 1
            elif index != expected:
                result.out_of_order += 1
            else:
                expected += 1

    finished = time.monotonic()

    # Linger briefly so our final ACKs reach the peer and it can finish cleanly.
    linger_end = time.monotonic() + 3 * link.cfg.rto_ms / 1000.0
    while time.monotonic() < linger_end:
        link.poll(now_ms())
        try:
            data = transport.read(0.001)
        except ConnectionError:
            break
        if data:
            link.input(data)
        link.frames()

    result.delivered = expected
    result.completed = expected == count
    result.elapsed_s = finished - start
    result.goodput_bps = expected * size / result.elapsed_s if result.elapsed_s > 0 else 0.0
    result.link_down_events = events["down"]
    result.stats = asdict(link.stats)
    return result
