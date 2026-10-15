#!/usr/bin/env python3
"""Generate the seed corpus for the fuzz harnesses.

Seeds are built from tests/vectors/frames.json plus hand-picked edge cases.
The fuzzer grows its own corpus from these. Committed seeds are replayed by
ctest on every build, so they double as regression tests.

Usage: python fuzz/make_corpus.py
"""
import json
import pathlib
import random
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests" / "vectors"))
from gen_vectors import crc8, encode, hexbytes  # noqa: E402

CORPUS = ROOT / "fuzz" / "corpus"
SYNC = 0x7E


def write(kind: str, name: str, data: bytes) -> None:
    path = CORPUS / kind / f"{name}.bin"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main() -> None:
    rng = random.Random(0xF1A3)
    vectors = json.loads((ROOT / "tests" / "vectors" / "frames.json").read_text(encoding="utf-8"))
    frames = [hexbytes(v["encoded"]) for v in vectors["frames"]]
    small_a = encode(0x01, 0x11, bytes(range(0x10, 0x90, 0x10)))
    small_b = encode(0x02, 0x22, b"frame")

    # Decoder seeds. The first byte is the control byte: low nibble sets the
    # feed chunk size, high nibble how often the timeout fires.
    for i, f in enumerate(frames):
        write("decoder", f"vector_{i:02d}_bytewise", bytes([0x00]) + f)
        write("decoder", f"vector_{i:02d}_chunked", bytes([0x2F]) + f)
    write("decoder", "back_to_back", bytes([0x07]) + b"".join(frames))
    write("decoder", "garbage_then_frame",
          bytes([0x03]) + bytes(rng.randrange(256) for _ in range(40)) + small_a)
    for cut in (1, 4, 5, 9):
        write("decoder", f"truncated_{cut}_then_frames", bytes([0x10]) + small_a[:cut] + small_b + small_a)
    bad_hdr = bytearray(small_a)
    bad_hdr[1] ^= 0x80
    write("decoder", "corrupt_len_then_frame", bytes([0x00]) + bytes(bad_hdr) + small_b)
    bad_crc = bytearray(small_a)
    bad_crc[-1] ^= 0x01
    write("decoder", "corrupt_crc_then_frame", bytes([0x00]) + bytes(bad_crc) + small_b)
    hdr = bytes([200, 0x01, 0x00])
    long_candidate = bytes([SYNC]) + hdr + bytes([crc8(hdr)])
    write("decoder", "frames_inside_long_candidate", bytes([0x05]) + long_candidate + small_a + small_b)
    write("decoder", "all_sync", bytes([0x00]) + bytes([SYNC]) * 300)

    # Round-trip seeds: TYPE SEQ SPLIT PAYLOAD...
    write("roundtrip", "empty", bytes([0x01, 0x00, 0x00]))
    write("roundtrip", "hello", bytes([0x01, 0x05, 0x03]) + b"hello")
    write("roundtrip", "sync_payload", bytes([0x10, 0xFF, 0x07]) + bytes([SYNC]) * 16)
    write("roundtrip", "control_ack", bytes([0xF0, 0x00, 0x01, 0x05, 0x04]))
    write("roundtrip", "max_payload", bytes([0x02, 0x80, 0x99]) + bytes(range(255)))
    write("roundtrip", "over_max", bytes([0x02, 0x81, 0x10]) + bytes(300))

    print(f"wrote {sum(1 for _ in CORPUS.rglob('*.bin'))} seeds to {CORPUS.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
