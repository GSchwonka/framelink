"""Tests for the Python link layer over a simulated faulty wire.

These mirror tests/test_link.c, so both implementations are held to the
same behaviour.
"""
import random

import pytest

from framelink import Busy, Config, Event, Link, NotConnected, encode
from framelink.codec import TYPE_ACK, TYPE_CREDIT, TYPE_HELLO, TYPE_HELLO_ACK, TYPE_NAK


class Endpoint:
    def __init__(self, name, rng, **cfg):
        self.name = name
        self.rng = rng
        self.wire = bytearray()  # bytes waiting to reach the peer
        self.drop_per_mille = 0
        self.corrupt_per_mille = 0
        self.cut = False
        self.drop_type = None
        self.drop_type_count = 0
        self.drop_type_len = None
        self.written = {}
        self.last_hello = b""
        self.events = []
        self.link = Link(self._write, Config(**cfg), self._event)

    def count(self, event):
        return sum(1 for e, _ in self.events if e == event)

    def last_arg(self, event):
        return [a for e, a in self.events if e == event][-1]

    def _event(self, event, arg):
        self.events.append((event, arg))

    def _write(self, frame):
        ftype = frame[2]
        self.written[ftype] = self.written.get(ftype, 0) + 1
        if ftype == TYPE_HELLO:
            self.last_hello = bytes(frame)
        if self.cut:
            return
        if (self.drop_type == ftype and self.drop_type_count > 0
                and (self.drop_type_len is None or frame[1] == self.drop_type_len)):
            self.drop_type_count -= 1
            return
        if self.drop_per_mille and self.rng.randrange(1000) < self.drop_per_mille:
            return
        out = bytearray(frame)
        if self.corrupt_per_mille:
            for i in range(len(out)):
                if self.rng.randrange(1000) < self.corrupt_per_mille:
                    out[i] ^= 1 << self.rng.randrange(8)
        self.wire += out


class Sim:
    def __init__(self, a=None, b=None, start=0):
        self.rng = random.Random(0x9E3779B9)
        base = dict(window=8, rx_slots=8, rto_ms=50, max_retries=10)
        self.a = Endpoint("A", self.rng, **{**base, "session_seed": 0x11, **(a or {})})
        self.b = Endpoint("B", self.rng, **{**base, "session_seed": 0x22, **(b or {})})
        self.now = start

    def tick(self):
        if self.a.wire:
            data, self.a.wire = bytes(self.a.wire), bytearray()
            self.b.link.input(data)
        if self.b.wire:
            data, self.b.wire = bytes(self.b.wire), bytearray()
            self.a.link.input(data)
        self.now = (self.now + 1) & 0xFFFFFFFF
        self.a.link.poll(self.now)
        self.b.link.poll(self.now)

    def run(self, ms):
        for _ in range(ms):
            self.tick()

    def connect(self):
        for _ in range(1000):
            if self.a.link.connected and self.b.link.connected:
                return
            self.tick()
        raise AssertionError("did not connect")

    def transfer(self, count, read_every=0, max_ms=20000):
        sent = received = ms = busy = 0
        while received < count:
            assert ms < max_ms, "transfer did not finish in time"
            assert self.a.link.connected, "link went down"
            while sent < count:
                try:
                    self.a.link.send(0x01, payload(sent))
                except Busy:
                    busy += 1
                    break
                sent += 1
            if read_every == 0 or ms % read_every == 0:
                while True:
                    f = self.b.link.recv()
                    if f is None:
                        break
                    assert f.payload == payload(received), "out of order or corrupted"
                    received += 1
                    if read_every:
                        break
            self.tick()
            ms += 1
        return ms, busy

    def finish(self):
        self.run(200)
        assert self.a.link.inflight == 0


def payload(i):
    return i.to_bytes(4, "little") + bytes((i * 31 + k) & 0xFF for k in range(i % 23))


def test_config_validation():
    for bad in (dict(window=0), dict(window=128), dict(rx_slots=0), dict(rx_slots=256),
                dict(rto_ms=0), dict(max_retries=0)):
        with pytest.raises(ValueError):
            Link(lambda b: None, Config(**bad))


def test_send_errors():
    s = Sim()
    with pytest.raises(NotConnected):
        s.a.link.send(0x01, b"x")
    s.connect()
    with pytest.raises(ValueError):
        s.a.link.send(TYPE_ACK, b"x")
    with pytest.raises(ValueError):
        s.a.link.send(0x01, bytes(256))
    for i in range(8):
        assert s.a.link.send(0x01, b"x") == i
    with pytest.raises(Busy):
        s.a.link.send(0x01, b"x")


def test_handshake_simultaneous_start():
    s = Sim()
    s.connect()
    s.run(500)
    assert s.a.count(Event.CONNECTED) == 1 and s.b.count(Event.CONNECTED) == 1
    assert s.a.count(Event.PEER_RESET) == 0 and s.b.count(Event.PEER_RESET) == 0


def test_handshake_survives_lost_hello_acks():
    s = Sim()
    s.a.drop_type = s.b.drop_type = TYPE_HELLO_ACK
    s.a.drop_type_count = s.b.drop_type_count = 3
    s.connect()
    s.transfer(50)
    s.finish()


def test_duplicate_hello_does_not_reset_link():
    s = Sim()
    s.connect()
    s.transfer(20)
    s.b.link.input(s.a.last_hello)
    s.transfer(20)
    s.finish()
    assert s.b.count(Event.PEER_RESET) == 0
    assert s.a.count(Event.DELIVERED) == 40


def test_lossless_transfer_and_wraparound():
    s = Sim(a=dict(window=127, rx_slots=127), b=dict(window=127, rx_slots=127))
    s.connect()
    s.transfer(2000)
    s.finish()
    assert s.a.count(Event.DELIVERED) == 2000
    assert s.a.link.stats.retransmits == 0


@pytest.mark.parametrize("drop,corrupt", [(10, 1), (50, 5), (100, 10)])
def test_lossy_channel(drop, corrupt):
    s = Sim(a=dict(rto_ms=30, max_retries=50), b=dict(rto_ms=30, max_retries=50))
    s.connect()
    for e in (s.a, s.b):
        e.drop_per_mille, e.corrupt_per_mille = drop, corrupt
    s.transfer(300, max_ms=60000)
    for e in (s.a, s.b):
        e.drop_per_mille = e.corrupt_per_mille = 0
    s.finish()
    assert s.a.count(Event.DELIVERED) == 300
    assert s.a.link.stats.link_down == 0


def test_corrupt_body_is_nakked_before_timeout():
    s = Sim(a=dict(rto_ms=2000), b=dict(rto_ms=2000))
    s.connect()
    s.a.link.send(0x01, bytes(16))
    s.a.wire[-3] ^= 0x01
    s.run(10)
    assert s.b.written.get(TYPE_NAK) == 1
    assert s.b.link.rx_pending == 1


def test_slow_consumer_never_overflows():
    s = Sim(a=dict(window=16, rx_slots=16), b=dict(window=16, rx_slots=4))
    s.connect()
    _, busy = s.transfer(200, read_every=7)
    s.finish()
    assert s.b.link.stats.overflows == 0
    assert s.a.link.stats.retransmits == 0
    assert busy > 0


def test_lost_credit_update_recovered_by_probe():
    s = Sim(b=dict(rx_slots=2))
    s.connect()
    s.a.link.send(0x01, b"1")
    s.a.link.send(0x01, b"2")
    s.run(5)
    with pytest.raises(Busy):
        s.a.link.send(0x01, b"3")
    s.b.drop_type, s.b.drop_type_count, s.b.drop_type_len = TYPE_CREDIT, 1, 2
    assert len(s.b.link.frames()) == 2
    for _ in range(200):
        try:
            s.a.link.send(0x01, b"3")
            break
        except Busy:
            s.tick()
    else:
        raise AssertionError("never recovered from the lost credit update")
    assert s.a.link.stats.link_down == 0


def test_link_down_and_reconnect():
    s = Sim()
    s.connect()
    s.a.cut = True
    for _ in range(3):
        s.a.link.send(0x01, b"x")
    s.run(600)
    assert s.a.count(Event.LINK_DOWN) == 1 and s.a.last_arg(Event.LINK_DOWN) == 3
    s.a.cut = False
    s.connect()
    assert s.b.count(Event.PEER_RESET) == 1
    s.transfer(50)
    s.finish()


def test_peer_restart_is_detected():
    s = Sim()
    s.connect()
    s.transfer(30)
    s.b = Endpoint("B", s.rng, window=8, rx_slots=8, rto_ms=50, max_retries=10, session_seed=0x23)
    s.connect()
    assert s.a.count(Event.PEER_RESET) == 1
    s.transfer(30)
    s.finish()


def test_stale_ack_is_ignored():
    s = Sim()
    s.connect()
    s.a.cut = True
    s.a.link.send(0x01, b"x")
    s.a.link.send(0x01, b"y")
    s.a.link.input(encode(TYPE_ACK, 0, bytes((200, 255))))
    assert s.a.link.inflight == 2 and s.a.link.peer_credits == 8


def test_ping_pong_and_version_mismatch():
    s = Sim()
    s.connect()
    s.a.link.ping(b"abc")
    s.run(3)
    assert s.a.last_arg(Event.PONG) == 3
    s.a.link.input(encode(TYPE_HELLO, 0, bytes((2, 8, 8, 0, 0x77))))
    assert s.a.last_arg(Event.VERSION_MISMATCH) == 2


def test_clock_wraparound():
    s = Sim(start=0xFFFFFF00)
    s.a.drop_per_mille = s.b.drop_per_mille = 30
    s.connect()
    s.transfer(200)
    s.a.drop_per_mille = s.b.drop_per_mille = 0
    s.finish()
    assert s.now < 0xFFFFFF00
