#!/usr/bin/env python3
"""Host recovery benchmark: Python host <-> C peer over TCP, with injected corruption.

For each corruption rate, this starts examples/tcp_peer (fl_peer), runs the
echo benchmark from the Python host through a NoisyTransport, and records
what both sides saw. It prints a Markdown table and can write a CSV.

The corruption model is the one in docs/MEASUREMENTS.md section 4: each byte,
in both directions, has one random bit flipped with probability r.

Usage:
    python bench/host_recovery.py --peer build/examples/fl_peer
    python bench/host_recovery.py --peer ... --runs 0:1000 0.01:1000 0.05:300 0.10:100 --csv out.csv

Against a real device running an echo peer, such as the ESP32 port:
    python bench/host_recovery.py --device serial:COM5@921600
    python bench/host_recovery.py --device tcp:192.168.1.50:5000
"""
import argparse
import csv
import json
import pathlib
import platform
import socket
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host" / "src"))

from framelink import Config  # noqa: E402
from framelink.bench import run_echo_bench  # noqa: E402
from framelink.transport import NoisyTransport, TcpTransport, open_transport  # noqa: E402

FIELDS = ["rate", "frames", "size", "delivered", "undetected", "out_of_order", "seconds",
          "goodput_Bps", "host_retransmits", "peer_retransmits", "hdr_errors", "crc_errors",
          "resyncs", "link_down", "peer_overflows", "bytes_corrupted"]


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run_one_device(spec: str, rate: float, frames: int, size: int, rto: int, seed: int) -> dict:
    """Run against a device that is already running an echo peer. Its own
    counters are not available, so only the host side is reported."""
    transport = open_transport(spec)
    noisy = NoisyTransport(transport, rate, seed=seed)
    result = run_echo_bench(noisy, frames, size, Config(rto_ms=rto, max_retries=255),
                            timeout_s=900, corrupt_rate=rate)
    transport.close()
    time.sleep(0.5)  # let the device notice the old session is gone
    host = result.stats
    return {
        "rate": rate, "frames": frames, "size": size, "delivered": result.delivered,
        "undetected": result.undetected, "out_of_order": result.out_of_order,
        "seconds": round(result.elapsed_s, 2), "goodput_Bps": round(result.goodput_bps),
        "host_retransmits": host["retransmits"], "peer_retransmits": "n/a",
        "hdr_errors": host["hdr_errors"], "crc_errors": host["crc_errors"], "resyncs": host["resyncs"],
        "link_down": result.link_down_events, "peer_overflows": 0, "bytes_corrupted": noisy.bytes_corrupted,
    }


def run_one(peer: str, rate: float, frames: int, size: int, rto: int, seed: int) -> dict:
    port = free_port()
    proc = subprocess.Popen([peer, "--listen", str(port), "--rto", str(rto), "--retries", "255",
                             "--idle-exit", "2000"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    line = proc.stderr.readline()
    if "listening" not in line:
        raise RuntimeError(f"fl_peer did not start: {line}")
    tcp = TcpTransport.connect("127.0.0.1", port)
    noisy = NoisyTransport(tcp, rate, seed=seed)
    result = run_echo_bench(noisy, frames, size, Config(rto_ms=rto, max_retries=255),
                            timeout_s=600, corrupt_rate=rate)
    tcp.close()
    out, err = proc.communicate(timeout=60)
    peer_stats = json.loads(out.strip().splitlines()[-1])
    host = result.stats
    return {
        "rate": rate, "frames": frames, "size": size, "delivered": result.delivered,
        "undetected": result.undetected, "out_of_order": result.out_of_order,
        "seconds": round(result.elapsed_s, 2), "goodput_Bps": round(result.goodput_bps),
        "host_retransmits": host["retransmits"], "peer_retransmits": peer_stats["retransmits"],
        "hdr_errors": host["hdr_errors"] + peer_stats["hdr_errors"],
        "crc_errors": host["crc_errors"] + peer_stats["crc_errors"],
        "resyncs": host["resyncs"] + peer_stats["resyncs"],
        "link_down": result.link_down_events + peer_stats["link_down"],
        "peer_overflows": peer_stats["overflows"], "bytes_corrupted": noisy.bytes_corrupted,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    target = ap.add_mutually_exclusive_group(required=True)
    target.add_argument("--peer", help="path to the fl_peer binary, started for each run")
    target.add_argument("--device", help="a running echo peer: serial:PORT@BAUD or tcp:HOST:PORT")
    ap.add_argument("--runs", nargs="+", default=["0:1000", "0.01:1000", "0.05:300", "0.10:100"],
                    help="RATE:FRAMES pairs")
    ap.add_argument("--size", type=int, default=16)
    ap.add_argument("--rto", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--csv", help="write results to this CSV file")
    args = ap.parse_args()

    rows = []
    for spec in args.runs:
        rate, _, frames = spec.partition(":")
        if args.device:
            rows.append(run_one_device(args.device, float(rate), int(frames), args.size, args.rto, args.seed))
        else:
            rows.append(run_one(args.peer, float(rate), int(frames), args.size, args.rto, args.seed))
        print(f"done: {spec}", file=sys.stderr, flush=True)

    where = args.device or "fl_peer over local TCP"
    print(f"Peer: {where}. Host: {platform.system()} {platform.machine()}, Python {platform.python_version()}, "
          f"{args.size}-byte payloads, rto {args.rto} ms, window 8, seed {args.seed}\n")
    print("| Corrupted bytes | Frames | Delivered | Undetected | Time (s) | Goodput (B/s) "
          "| Retransmits (host / peer) | Header + CRC errors | Resyncs | Link down |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        print(f"| {r['rate']:.0%} | {r['frames']} | {r['delivered']} | {r['undetected']} | {r['seconds']} "
              f"| {r['goodput_Bps']} | {r['host_retransmits']} / {r['peer_retransmits']} "
              f"| {r['hdr_errors'] + r['crc_errors']} | {r['resyncs']} | {r['link_down']} |")

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=FIELDS)
            w.writeheader()
            w.writerows(rows)

    ok = all(r["delivered"] == r["frames"] and r["undetected"] == 0 and r["out_of_order"] == 0
             and r["peer_overflows"] == 0 for r in rows)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
