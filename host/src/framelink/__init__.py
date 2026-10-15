"""framelink: reliable framed messaging over unreliable byte streams.

Python reference implementation of the framelink protocol, wire-compatible
with the C library. See docs/SPEC.md in the repository.

SPDX-License-Identifier: MIT
Copyright (c) 2026 Guilherme Schwonka
"""
from .codec import (
    MAX_PAYLOAD,
    OVERHEAD,
    PROTOCOL_VERSION,
    SYNC,
    Decoder,
    DecoderStats,
    Frame,
    crc8,
    crc16,
    encode,
)
from .link import Busy, Config, Event, Link, LinkError, NotConnected, Stats

__version__ = "0.2.0"

__all__ = [
    "MAX_PAYLOAD",
    "OVERHEAD",
    "PROTOCOL_VERSION",
    "SYNC",
    "Busy",
    "Config",
    "Decoder",
    "DecoderStats",
    "Event",
    "Frame",
    "Link",
    "LinkError",
    "NotConnected",
    "Stats",
    "crc8",
    "crc16",
    "encode",
]
