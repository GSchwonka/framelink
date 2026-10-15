"""Interoperability tests: the Python host against the C library over TCP.

The C side is examples/tcp_peer/fl_peer.c, which echoes every frame. Set
FL_PEER to its path to run these tests. CI does this on every push.
Without FL_PEER they are skipped.
"""
import json
import os
import socket
import subprocess

import pytest

from framelink import Config
from framelink.bench import run_echo_bench
from framelink.transport import NoisyTransport, TcpTransport

FL_PEER = os.environ.get("FL_PEER")
pytestmark = pytest.mark.skipif(not FL_PEER, reason="set FL_PEER to the fl_peer binary")


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Peer:
    """Run fl_peer, connect to it, and collect its statistics on exit."""

    def __init__(self, *args):
        self.port = free_port()
        self.proc = subprocess.Popen(
            [FL_PEER, "--listen", str(self.port), "--idle-exit", "1000", *map(str, args)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        line = self.proc.stderr.readline()
        assert "listening" in line, line
        self.transport = TcpTransport.connect("127.0.0.1", self.port)

    def finish(self) -> dict:
        self.transport.close()  # the peer exits when the connection closes
        out, err = self.proc.communicate(timeout=30)
        assert self.proc.returncode == 0, err
        return json.loads(out.strip().splitlines()[-1])


def test_echo_clean_link():
    peer = Peer("--rto", 20)
    result = run_echo_bench(peer.transport, 500, size=32, config=Config(rto_ms=20), timeout_s=60)
    stats = peer.finish()
    assert result.completed and result.undetected == 0 and result.out_of_order == 0
    assert result.stats["retransmits"] == 0
    assert stats["echoed"] == 500
    assert stats["overflows"] == 0 and stats["crc_errors"] == 0


def test_echo_max_payload():
    peer = Peer("--rto", 50)
    result = run_echo_bench(peer.transport, 50, size=255, config=Config(rto_ms=50), timeout_s=60)
    peer.finish()
    assert result.completed and result.undetected == 0


def test_echo_through_corruption():
    peer = Peer("--rto", 20, "--retries", 255)
    noisy = NoisyTransport(peer.transport, 0.02, seed=7)
    result = run_echo_bench(noisy, 200, size=16, config=Config(rto_ms=20, max_retries=255),
                            timeout_s=120, corrupt_rate=0.02)
    stats = peer.finish()
    assert noisy.bytes_corrupted > 0
    assert result.completed, result
    assert result.undetected == 0 and result.out_of_order == 0
    assert result.link_down_events == 0 and stats["link_down"] == 0
    assert stats["crc_errors"] + stats["hdr_errors"] > 0


def test_slow_c_consumer_is_protected_by_credits():
    # The C side has 2 slots and takes one frame every 5 ms, far slower than
    # Python can send. Credits must hold Python back: no overflow, ever.
    peer = Peer("--slots", 2, "--consume-every", 5, "--rto", 50)
    result = run_echo_bench(peer.transport, 100, size=16,
                            config=Config(window=16, rx_slots=16, rto_ms=50), timeout_s=60)
    stats = peer.finish()
    assert result.completed
    assert result.busy_refusals > 0
    assert stats["overflows"] == 0
    assert result.stats["retransmits"] == 0
