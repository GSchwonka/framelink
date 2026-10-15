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

__version__ = "0.2.0"

__all__ = [
    "MAX_PAYLOAD",
    "OVERHEAD",
    "PROTOCOL_VERSION",
    "SYNC",
    "Decoder",
    "DecoderStats",
    "Frame",
    "crc8",
    "crc16",
    "encode",
]
