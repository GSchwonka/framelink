"""framelink codec: checksums, frame encoder and resynchronising decoder.

This is the Python reference implementation of docs/SPEC.md sections 2, 3
and 6. It is byte-for-byte compatible with the C library and is tested
against the same vectors in tests/vectors/frames.json.

SPDX-License-Identifier: MIT
Copyright (c) 2026 Guilherme Schwonka
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, List, Optional

SYNC = 0x7E
HEADER_LEN = 5  # SYNC LEN TYPE SEQ HCRC
TRAILER_LEN = 2  # CRC_LO CRC_HI
OVERHEAD = HEADER_LEN + TRAILER_LEN
MAX_PAYLOAD = 255
PROTOCOL_VERSION = 1

TYPE_USER_MAX = 0xEF
TYPE_ACK = 0xF0
TYPE_NAK = 0xF1
TYPE_CREDIT = 0xF2
TYPE_HELLO = 0xF3
TYPE_HELLO_ACK = 0xF4
TYPE_PING = 0xF5
TYPE_PONG = 0xF6


def _make_crc16_table() -> List[int]:
    table = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
        table.append(crc)
    return table


_CRC16_TABLE = _make_crc16_table()


def crc8(data: bytes) -> int:
    """CRC-8/SMBUS: poly 0x07, init 0x00, no reflection, no final XOR."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR."""
    table = _CRC16_TABLE
    for byte in data:
        crc = ((crc << 8) & 0xFFFF) ^ table[((crc >> 8) ^ byte) & 0xFF]
    return crc


def frame_size(payload_len: int) -> int:
    return payload_len + OVERHEAD


def encode(frame_type: int, seq: int, payload: bytes = b"", max_payload: int = MAX_PAYLOAD) -> bytes:
    """Encode one frame. Raises ValueError for out-of-range fields."""
    if not 0 <= frame_type <= 0xFF or not 0 <= seq <= 0xFF:
        raise ValueError("type and seq must be single bytes")
    if len(payload) > max_payload:
        raise ValueError(f"payload of {len(payload)} bytes exceeds {max_payload}")
    header = bytes((len(payload), frame_type, seq))
    header += bytes((crc8(header),))
    crc = crc16(header + payload)
    return bytes((SYNC,)) + header + bytes(payload) + bytes((crc & 0xFF, crc >> 8))


@dataclass(frozen=True)
class Frame:
    """A decoded frame."""

    type: int
    seq: int
    payload: bytes


@dataclass
class DecoderStats:
    """Decoder counters, named as in docs/SPEC.md section 8."""

    rx_frames: int = 0
    hunt_bytes: int = 0
    hdr_errors: int = 0
    crc_errors: int = 0
    resyncs: int = 0
    timeouts: int = 0


@dataclass
class Decoder:
    """Byte-stream decoder that never loses a valid frame to a corrupted one.

    When a candidate frame fails either CRC, only its first byte is dropped
    and the remaining buffered bytes are scanned again, exactly like the C
    implementation.
    """

    max_payload: int = MAX_PAYLOAD
    on_corrupt: Optional[Callable[[Frame], None]] = None
    stats: DecoderStats = field(default_factory=DecoderStats)

    def __post_init__(self) -> None:
        if not 0 <= self.max_payload <= MAX_PAYLOAD:
            raise ValueError("max_payload must be 0..255")
        self._buf = bytearray()
        self._need = 0
        self._resync_pending = False

    @property
    def busy(self) -> bool:
        """True while a partial frame is buffered."""
        return len(self._buf) > 0

    def feed(self, data: bytes) -> List[Frame]:
        """Feed received bytes and return the complete, valid frames found."""
        out: List[Frame] = []
        buf = self._buf
        for byte in data:
            if not buf and byte != SYNC:
                self.stats.hunt_bytes += 1
                continue
            buf.append(byte)
            self._process(out)
        return out

    def timeout(self) -> List[Frame]:
        """Abandon a partial frame after an inter-byte timeout and rescan it."""
        out: List[Frame] = []
        if not self._buf:
            return out
        self.stats.timeouts += 1
        while self._buf:
            self._reject()
            self._process(out)
        return out

    # Internal state machine, mirroring dec_process in framelink.c.

    def _consume(self, count: int) -> None:
        del self._buf[:count]
        self._need = 0

    def _reject(self) -> None:
        self._resync_pending = True
        self._consume(1)

    def _process(self, out: List[Frame]) -> None:
        buf = self._buf
        while True:
            if buf and buf[0] != SYNC:
                skip = buf.find(SYNC, 1)
                skip = len(buf) if skip < 0 else skip
                self.stats.hunt_bytes += skip
                self._consume(skip)
            if not buf:
                self._resync_pending = False
                return

            if self._need == 0:
                if len(buf) < HEADER_LEN:
                    return
                if crc8(buf[1:4]) != buf[4] or buf[1] > self.max_payload:
                    self.stats.hdr_errors += 1
                    self._reject()
                    continue
                self._need = buf[1] + OVERHEAD

            need = self._need
            if len(buf) < need:
                return

            received = buf[need - 2] | (buf[need - 1] << 8)
            if crc16(bytes(buf[1:need - 2])) != received:
                self.stats.crc_errors += 1
                if self.on_corrupt is not None:
                    self.on_corrupt(Frame(buf[2], buf[3], bytes(buf[HEADER_LEN:need - 2])))
                self._reject()
                continue

            self.stats.rx_frames += 1
            if self._resync_pending:
                self.stats.resyncs += 1
                self._resync_pending = False
            out.append(Frame(buf[2], buf[3], bytes(buf[HEADER_LEN:need - 2])))
            self._consume(need)
