"""framelink link layer: handshake, Go-Back-N ARQ and credit-based flow control.

This is the Python reference implementation of docs/SPEC.md section 5. It
mirrors framelink.c function by function, so the two can be read side by
side, and it interoperates with the C library on the wire.

Like the C version it owns no clock and no thread: call poll() often with a
millisecond timestamp, and give it a write function for outgoing bytes.

SPDX-License-Identifier: MIT
Copyright (c) 2026 Guilherme Schwonka
"""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from enum import IntEnum
from typing import Callable, Deque, List, Optional

from .codec import (
    MAX_PAYLOAD,
    PROTOCOL_VERSION,
    TYPE_ACK,
    TYPE_CREDIT,
    TYPE_HELLO,
    TYPE_HELLO_ACK,
    TYPE_NAK,
    TYPE_PING,
    TYPE_PONG,
    TYPE_USER_MAX,
    Decoder,
    Frame,
    encode,
)

PING_MAX = 8
HELLO_LEN = 5
HELLO_ACK_LEN = 6


class Event(IntEnum):
    """Things the link reports through `on_event`. Values match fl_event in C."""

    CONNECTED = 1  # handshake complete, arg 0
    DELIVERED = 2  # the peer acknowledged a data frame, arg = its SEQ
    LINK_DOWN = 3  # retries exhausted, arg = frames never acknowledged
    PEER_RESET = 4  # the peer started a new session, arg = frames never acknowledged
    PONG = 5  # a PONG arrived, arg = its payload length
    VERSION_MISMATCH = 6  # the peer uses another protocol version, arg = that version


class LinkError(Exception):
    """Base class for link errors."""


class Busy(LinkError):
    """The window is full or the peer has no free slots. Try again later."""


class NotConnected(LinkError):
    """The handshake has not completed."""


@dataclass
class Config:
    window: int = 8  # frames in flight, 1..127
    rx_slots: int = 8  # receive queue size, 1..255
    rto_ms: int = 50  # retransmit and handshake interval
    max_retries: int = 10  # retransmissions of one frame before link down
    frame_timeout_ms: int = 0  # inter-byte timeout, 0 = rto_ms
    session_seed: int = 0  # first session ID, use a random value
    max_payload: int = MAX_PAYLOAD


@dataclass
class Stats:
    """Link and decoder counters, named as in docs/SPEC.md section 8."""

    rx_frames: int = 0
    tx_frames: int = 0
    hunt_bytes: int = 0
    hdr_errors: int = 0
    crc_errors: int = 0
    resyncs: int = 0
    timeouts: int = 0
    retransmits: int = 0
    dup_drops: int = 0
    overflows: int = 0
    unknown_type: int = 0
    link_down: int = 0


def _due(now: int, deadline: int) -> bool:
    """True once `deadline` is reached, correct across 32-bit clock wraparound."""
    return ((now - deadline) & 0xFFFFFFFF) < 0x80000000


class Link:
    """One end of a framelink connection."""

    def __init__(
        self,
        write: Callable[[bytes], object],
        config: Optional[Config] = None,
        on_event: Optional[Callable[[Event, int], None]] = None,
    ) -> None:
        cfg = config or Config()
        if not 1 <= cfg.window <= 127:
            raise ValueError("window must be 1..127")
        if not 1 <= cfg.rx_slots <= 255:
            raise ValueError("rx_slots must be 1..255")
        if cfg.rto_ms <= 0 or cfg.max_retries <= 0:
            raise ValueError("rto_ms and max_retries must be positive")
        self.cfg = cfg
        self._write_fn = write
        self._on_event = on_event
        self._decoder = Decoder(max_payload=cfg.max_payload, on_corrupt=self._on_corrupt)

        self.now = 0
        self._last_rx = 0

        self.connected = False
        self.session = cfg.session_seed & 0xFF
        self.peer_session = 0
        self._peer_session_valid = False
        self._hello_now = True
        self._hello_due = 0

        self.snd_una = 0
        self.snd_nxt = 0
        self._tx: Deque[bytes] = deque()  # encoded frames in flight, oldest first
        self.peer_credits = 0
        self.retries = 0
        self._timer_on = False
        self._rto_due = 0
        self._want_credit = False
        self._probe_due = 0

        self.rcv_nxt = 0
        self._rx: Deque[Frame] = deque()
        self._nak_sent = False
        self._zero_advertised = False

        self._tx_frames = 0
        self._retransmits = 0
        self._dup_drops = 0
        self._overflows = 0
        self._unknown_type = 0
        self._link_downs = 0

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def poll(self, now_ms: int) -> None:
        """Run timers: HELLO, retransmissions, probes and the frame timeout."""
        now = now_ms & 0xFFFFFFFF
        self.now = now
        frame_timeout = self.cfg.frame_timeout_ms or self.cfg.rto_ms

        if self._decoder.busy and _due(now, (self._last_rx + frame_timeout) & 0xFFFFFFFF):
            for f in self._decoder.timeout():
                self._on_frame(f)

        if not self.connected:
            if self._hello_now or _due(now, self._hello_due):
                self._hello_now = False
                self._hello_due = (now + self.cfg.rto_ms) & 0xFFFFFFFF
                self._send_hello(TYPE_HELLO, 0)
            return

        if self._timer_on and _due(now, self._rto_due):
            if self.retries >= self.cfg.max_retries:
                self._link_down()
                return
            self.retries += 1
            self._retransmit_all()

        if (self._want_credit and self.peer_credits == 0 and self.inflight == 0
                and _due(now, self._probe_due)):
            self._probe_due = (now + self.cfg.rto_ms) & 0xFFFFFFFF
            self._send_control(TYPE_CREDIT, b"")

    def input(self, data: bytes) -> None:
        """Feed received bytes."""
        if data:
            self._last_rx = self.now
            for f in self._decoder.feed(data):
                self._on_frame(f)

    def send(self, frame_type: int, payload: bytes = b"") -> int:
        """Send one data frame and return its SEQ.

        Raises Busy when the window is full or the peer has no credits, and
        NotConnected before the handshake completes.
        """
        if not 0 <= frame_type <= TYPE_USER_MAX:
            raise ValueError("data frame type must be 0x00..0xEF")
        if len(payload) > self.cfg.max_payload:
            raise ValueError("payload too long")
        if not self.connected:
            raise NotConnected()
        inflight = self.inflight
        if inflight >= self.cfg.window:
            raise Busy("window full")
        if inflight >= self.peer_credits:
            if not self._want_credit:
                self._want_credit = True
                self._probe_due = (self.now + self.cfg.rto_ms) & 0xFFFFFFFF
            raise Busy("peer has no free slots")

        seq = self.snd_nxt
        frame = encode(frame_type, seq, bytes(payload), self.cfg.max_payload)
        self._tx.append(frame)
        self.snd_nxt = (self.snd_nxt + 1) & 0xFF
        self._write(frame)
        if not self._timer_on:
            self._timer_on = True
            self._rto_due = (self.now + self.cfg.rto_ms) & 0xFFFFFFFF
        return seq

    def recv(self) -> Optional[Frame]:
        """Take the oldest received frame, or None. Frees a credit for the peer."""
        if not self._rx:
            return None
        frame = self._rx.popleft()
        if self._zero_advertised and self.connected:
            self._send_position(TYPE_CREDIT, (self.rcv_nxt - 1) & 0xFF)
        return frame

    def ping(self, payload: bytes = b"") -> None:
        if len(payload) > PING_MAX:
            raise ValueError("PING payload is at most 8 bytes")
        self._send_control(TYPE_PING, bytes(payload))

    @property
    def inflight(self) -> int:
        return (self.snd_nxt - self.snd_una) & 0xFF

    @property
    def rx_pending(self) -> int:
        return len(self._rx)

    @property
    def stats(self) -> Stats:
        d = self._decoder.stats
        return Stats(
            rx_frames=d.rx_frames, tx_frames=self._tx_frames, hunt_bytes=d.hunt_bytes,
            hdr_errors=d.hdr_errors, crc_errors=d.crc_errors, resyncs=d.resyncs,
            timeouts=d.timeouts, retransmits=self._retransmits, dup_drops=self._dup_drops,
            overflows=self._overflows, unknown_type=self._unknown_type, link_down=self._link_downs,
        )

    # ------------------------------------------------------------------
    # Sending helpers
    # ------------------------------------------------------------------

    def _emit(self, event: Event, arg: int) -> None:
        if self._on_event is not None:
            self._on_event(event, arg)

    def _free_slots(self) -> int:
        return self.cfg.rx_slots - len(self._rx)

    def _write(self, frame: bytes) -> None:
        self._write_fn(frame)
        self._tx_frames += 1

    def _send_control(self, frame_type: int, payload: bytes) -> None:
        self._write(encode(frame_type, 0, payload))

    def _send_position(self, frame_type: int, seq: int) -> None:
        credits = self._free_slots()
        self._zero_advertised = credits == 0
        self._send_control(frame_type, bytes((seq, credits)))

    def _send_ack(self) -> None:
        self._send_position(TYPE_ACK, (self.rcv_nxt - 1) & 0xFF)

    def _send_hello(self, frame_type: int, echo: int) -> None:
        credits = self._free_slots()
        self._zero_advertised = credits == 0
        payload = bytes((PROTOCOL_VERSION, self.cfg.window, credits, 0, self.session, echo))
        self._send_control(frame_type, payload[:HELLO_LEN] if frame_type == TYPE_HELLO else payload)

    def _retransmit_all(self) -> None:
        for frame in self._tx:
            self._write(frame)
            self._retransmits += 1
        self._timer_on = bool(self._tx)
        self._rto_due = (self.now + self.cfg.rto_ms) & 0xFFFFFFFF

    # ------------------------------------------------------------------
    # State changes
    # ------------------------------------------------------------------

    def _reset_sequences(self) -> int:
        dropped = self.inflight
        self.snd_una = self.snd_nxt = 0
        self._tx.clear()
        self.peer_credits = 0
        self.retries = 0
        self._timer_on = False
        self._want_credit = False
        self.rcv_nxt = 0
        self._nak_sent = False
        return dropped

    def _link_down(self) -> None:
        dropped = self._reset_sequences()
        self.connected = False
        self.session = (self.session + 1) & 0xFF
        self._peer_session_valid = False
        self._hello_now = True
        self._link_downs += 1
        self._emit(Event.LINK_DOWN, dropped)

    def _connect(self, peer_credits: int) -> None:
        self.connected = True
        self.peer_credits = peer_credits
        self._emit(Event.CONNECTED, 0)

    # ------------------------------------------------------------------
    # Receiving
    # ------------------------------------------------------------------

    def _on_hello(self, f: Frame) -> None:
        p = f.payload
        if len(p) < HELLO_LEN:
            return
        if p[0] != PROTOCOL_VERSION:
            self._emit(Event.VERSION_MISMATCH, p[0])
            return
        peer_session = p[4]
        if self._peer_session_valid and peer_session == self.peer_session:
            self._send_hello(TYPE_HELLO_ACK, peer_session)  # retransmission
            return
        was_connected = self.connected
        dropped = self._reset_sequences()
        self.peer_session = peer_session
        self._peer_session_valid = True
        self._send_hello(TYPE_HELLO_ACK, peer_session)
        if was_connected:
            self._emit(Event.PEER_RESET, dropped)
        self._connect(p[2])

    def _on_hello_ack(self, f: Frame) -> None:
        p = f.payload
        if len(p) < HELLO_ACK_LEN:
            return
        if p[0] != PROTOCOL_VERSION:
            self._emit(Event.VERSION_MISMATCH, p[0])
            return
        if p[5] != self.session or self.connected:
            return
        self.peer_session = p[4]
        self._peer_session_valid = True
        self._connect(p[2])

    def _on_position(self, ack_seq: int, credits: int, is_nak: bool) -> None:
        inflight = self.inflight
        acked = (ack_seq + 1 - self.snd_una) & 0xFF
        if acked > inflight:
            return  # stale or invalid: ignore it, credits included
        first = self.snd_una
        self.snd_una = (self.snd_una + acked) & 0xFF
        for _ in range(acked):
            self._tx.popleft()
        self.peer_credits = credits
        if credits > 0:
            self._want_credit = False
        if acked:
            self.retries = 0
            self._timer_on = self.inflight > 0
            self._rto_due = (self.now + self.cfg.rto_ms) & 0xFFFFFFFF
        if is_nak and self.inflight:
            self._retransmit_all()
        for i in range(acked):
            self._emit(Event.DELIVERED, (first + i) & 0xFF)

    def _on_data(self, f: Frame) -> None:
        distance = (f.seq - self.rcv_nxt) & 0xFF
        if distance == 0:
            if len(self._rx) < self.cfg.rx_slots:
                self._rx.append(f)
                self.rcv_nxt = (self.rcv_nxt + 1) & 0xFF
                self._nak_sent = False
            else:
                self._overflows += 1
            self._send_ack()
        elif distance >= 128:
            self._dup_drops += 1
            self._send_ack()
        elif not self._nak_sent:
            self._nak_sent = True
            self._send_position(TYPE_NAK, self.rcv_nxt)

    def _on_frame(self, f: Frame) -> None:
        t = f.type
        if t == TYPE_HELLO:
            self._on_hello(f)
            return
        if t == TYPE_HELLO_ACK:
            self._on_hello_ack(f)
            return
        if t == TYPE_PING:
            if len(f.payload) <= PING_MAX:
                self._send_control(TYPE_PONG, f.payload)
            return
        if t == TYPE_PONG:
            self._emit(Event.PONG, len(f.payload))
            return
        if t > TYPE_PONG:
            self._unknown_type += 1
            return
        if not self.connected:
            return
        if t in (TYPE_ACK, TYPE_NAK):
            if len(f.payload) == 2:
                seq = f.payload[0]
                if t == TYPE_NAK:
                    seq = (seq - 1) & 0xFF
                self._on_position(seq, f.payload[1], t == TYPE_NAK)
            return
        if t == TYPE_CREDIT:
            if not f.payload:
                self._send_position(TYPE_CREDIT, (self.rcv_nxt - 1) & 0xFF)
            elif len(f.payload) == 2:
                self._on_position(f.payload[0], f.payload[1], False)
            return
        self._on_data(f)

    def _on_corrupt(self, f: Frame) -> None:
        if (self.connected and f.type <= TYPE_USER_MAX and not self._nak_sent
                and ((f.seq - self.rcv_nxt) & 0xFF) < 128):
            self._nak_sent = True
            self._send_position(TYPE_NAK, self.rcv_nxt)

    def frames(self) -> List[Frame]:
        """Drain and return every queued frame."""
        out = []
        while True:
            f = self.recv()
            if f is None:
                return out
            out.append(f)
