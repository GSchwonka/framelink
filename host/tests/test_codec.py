"""Tests for the Python codec, using the vectors shared with the C tests."""
import itertools
import json
import pathlib
import random

import pytest

from framelink import Decoder, Frame, crc8, crc16, encode
from framelink.codec import OVERHEAD, SYNC

VECTORS = json.loads(
    (pathlib.Path(__file__).resolve().parents[2] / "tests" / "vectors" / "frames.json").read_text()
)
FRAME_A = encode(0x01, 0x11, bytes(range(0x10, 0x90, 0x10)))
FRAME_B = encode(0x02, 0x22, b"frame")


def h(s: str) -> bytes:
    return bytes.fromhex(s.replace(" ", ""))


def test_crc_check_values():
    check = VECTORS["crc_check"]
    data = check["input"].encode()
    assert crc8(data) == int(check["crc8"], 16)
    assert crc16(data) == int(check["crc16"], 16)


@pytest.mark.parametrize("v", VECTORS["frames"], ids=lambda v: v["name"])
def test_vector_encode_and_decode(v):
    ftype, seq, payload = int(v["type"], 16), int(v["seq"], 16), h(v["payload"])
    assert encode(ftype, seq, payload) == h(v["encoded"])
    assert Decoder().feed(h(v["encoded"])) == [Frame(ftype, seq, payload)]


def test_encode_rejects_bad_arguments():
    with pytest.raises(ValueError):
        encode(0x01, 0, bytes(256))
    with pytest.raises(ValueError):
        encode(0x100, 0)
    with pytest.raises(ValueError):
        encode(0x01, 0, bytes(65), max_payload=64)


def test_back_to_back_frames_split_at_every_boundary():
    stream = FRAME_A + FRAME_B
    for split in range(len(stream) + 1):
        d = Decoder()
        frames = d.feed(stream[:split]) + d.feed(stream[split:])
        assert [f.seq for f in frames] == [0x11, 0x22]


def test_corrupt_header_does_not_delay_next_frame():
    for byte in range(1, 5):
        for bit in range(8):
            bad = bytearray(FRAME_A)
            bad[byte] ^= 1 << bit
            d = Decoder()
            assert d.feed(bytes(bad) + FRAME_B) == [Frame(0x02, 0x22, b"frame")]
            assert d.stats.hdr_errors == 1
            assert not d.busy


def test_truncated_frame_followed_by_valid_frames():
    for cut in range(1, len(FRAME_A)):
        d = Decoder()
        frames = d.feed(FRAME_A[:cut] + FRAME_B + FRAME_A) + d.timeout()
        assert [f.seq for f in frames] == [0x22, 0x11]


def test_frames_inside_failed_candidate_are_recovered():
    hdr = bytes((200, 0x01, 0x00))
    stream = bytes((SYNC,)) + hdr + bytes((crc8(hdr),)) + FRAME_A + FRAME_B
    d = Decoder()
    assert d.feed(stream) == []
    assert [f.seq for f in d.timeout()] == [0x11, 0x22]
    assert d.stats.resyncs == 1 and d.stats.timeouts == 1


def test_every_1_and_2_bit_error_is_detected():
    good = encode(0x09, 0x42, bytes(random.Random(7).randrange(256) for _ in range(16)))
    nbits = len(good) * 8
    for k in (1, 2):
        for bits in itertools.combinations(range(nbits), k):
            e = bytearray(good)
            for b in bits:
                e[b // 8] ^= 1 << (b % 8)
            d = Decoder()
            assert d.feed(bytes(e)) + d.timeout() == [], bits


def test_property_corrupting_channel():
    rng = random.Random(0x2545F491)
    sent, intact, stream = [], [], bytearray()
    for i in range(2000):
        payload = bytes(rng.randrange(256) for _ in range(rng.randrange(41)))
        frame = bytearray(encode(0x01, i & 0xFF, payload))
        ok = True
        for j in range(len(frame)):
            if rng.randrange(50) == 0:
                frame[j] ^= 1 << rng.randrange(8)
                ok = False
        sent.append(Frame(0x01, i & 0xFF, payload))
        intact.append(ok)
        stream += frame
    d = Decoder()
    got = d.feed(bytes(stream)) + d.timeout()

    i = 0
    for f in got:
        while i < len(sent) and sent[i] != f:
            assert not intact[i], "an intact frame was skipped"
            i += 1
        assert i < len(sent), "delivered frame matches no sent frame"
        i += 1
    assert not any(intact[i:]), "an intact frame at the end was lost"
    assert sum(intact) > 500


def test_max_payload_limit_rejects_long_header():
    d = Decoder(max_payload=64)
    frame = encode(0x01, 0, bytes(100))
    assert d.feed(frame) + d.timeout() == []
    assert d.stats.hdr_errors >= 1


def test_overhead_constant():
    assert len(encode(0x01, 0)) == OVERHEAD
