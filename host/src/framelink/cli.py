"""Command-line tool for framelink.

    framelink monitor TRANSPORT          decode a raw byte stream and print frames
    framelink send TRANSPORT MESSAGE...  connect, send each message, wait for delivery
    framelink recv TRANSPORT             connect and print every frame received
    framelink bench TRANSPORT            echo benchmark against fl_peer
    framelink noisy --listen PORT --target HOST:PORT --corrupt RATE
                                         TCP proxy that flips bits in both directions

TRANSPORT is tcp:HOST:PORT, listen:PORT or serial:PORT@BAUD.

SPDX-License-Identifier: MIT
Copyright (c) 2026 Guilherme Schwonka
"""
from __future__ import annotations

import argparse
import json
import random
import select
import socket
import sys
import time
from typing import List, Optional

from . import __version__
from .bench import run_echo_bench
from .codec import (
    TYPE_ACK,
    TYPE_CREDIT,
    TYPE_HELLO,
    TYPE_HELLO_ACK,
    TYPE_NAK,
    TYPE_PING,
    TYPE_PONG,
    Decoder,
)
from .link import Busy, Config, Event, Link
from .transport import NoisyTransport, open_transport

TYPE_NAMES = {
    TYPE_ACK: "ACK", TYPE_NAK: "NAK", TYPE_CREDIT: "CREDIT", TYPE_HELLO: "HELLO",
    TYPE_HELLO_ACK: "HELLO_ACK", TYPE_PING: "PING", TYPE_PONG: "PONG",
}


def _config(args) -> Config:
    return Config(window=args.window, rx_slots=args.slots, rto_ms=args.rto,
                  max_retries=args.retries, session_seed=random.randrange(256))


def _connect(args, on_event=None):
    transport = open_transport(args.transport)
    start = time.monotonic()
    link = Link(transport.write, _config(args), on_event)

    def step(wait: float = 0.001) -> None:
        link.poll(int((time.monotonic() - start) * 1000))
        data = transport.read(wait)
        if data:
            link.input(data)

    deadline = time.monotonic() + args.timeout
    while not link.connected:
        if time.monotonic() > deadline:
            raise SystemExit("framelink: handshake timed out")
        step()
    return transport, link, step


def cmd_monitor(args) -> int:
    transport = open_transport(args.transport)
    decoder = Decoder()
    start = time.monotonic()
    try:
        while True:
            try:
                data = transport.read(0.1)
            except ConnectionError:
                break
            for f in decoder.feed(data):
                name = TYPE_NAMES.get(f.type, f"0x{f.type:02X}")
                print(f"{time.monotonic() - start:9.3f}  {name:<9} seq={f.seq:3d} "
                      f"len={len(f.payload):3d}  {f.payload.hex(' ')}")
    except KeyboardInterrupt:
        pass
    print(json.dumps(decoder.stats.__dict__), file=sys.stderr)
    return 0


def cmd_send(args) -> int:
    pending = {}

    def on_event(ev: Event, arg: int) -> None:
        if ev is Event.DELIVERED:
            pending.pop(arg, None)
        elif ev is Event.LINK_DOWN:
            raise SystemExit(f"framelink: link down, {arg} frame(s) not acknowledged")

    transport, link, step = _connect(args, on_event)
    for message in args.message:
        while True:
            try:
                seq = link.send(args.type, message.encode())
                pending[seq] = message
                break
            except Busy:
                step()
    deadline = time.monotonic() + args.timeout
    while pending and time.monotonic() < deadline:
        step()
    transport.close()
    if pending:
        print(f"framelink: {len(pending)} message(s) not acknowledged", file=sys.stderr)
        return 1
    print(f"delivered {len(args.message)} message(s)")
    return 0


def cmd_recv(args) -> int:
    transport, link, step = _connect(args)
    received = 0
    try:
        while args.count == 0 or received < args.count:
            try:
                step(0.01)
            except ConnectionError:
                break
            for f in link.frames():
                received += 1
                text = f.payload.decode("utf-8", "replace") if args.text else f.payload.hex(" ")
                print(f"type=0x{f.type:02X} seq={f.seq:3d} {text}", flush=True)
    except KeyboardInterrupt:
        pass
    transport.close()
    return 0


def cmd_bench(args) -> int:
    transport = open_transport(args.transport)
    if args.corrupt:
        transport = NoisyTransport(transport, args.corrupt, seed=args.seed)
    result = run_echo_bench(transport, args.count, args.size, _config(args),
                            timeout_s=args.timeout, corrupt_rate=args.corrupt)
    transport.close()
    if args.json:
        print(json.dumps(result.as_dict()))
    else:
        s = result.stats
        print(f"delivered {result.delivered}/{result.count} frames of {result.size} B "
              f"in {result.elapsed_s:.2f} s, goodput {result.goodput_bps:.0f} B/s")
        print(f"corrupt rate {result.corrupt_rate:.1%}, retransmits {s['retransmits']}, "
              f"crc errors {s['crc_errors']}, header errors {s['hdr_errors']}, resyncs {s['resyncs']}, "
              f"link down {result.link_down_events}, undetected {result.undetected}")
    return 0 if result.completed and result.undetected == 0 and result.out_of_order == 0 else 1


def cmd_noisy(args) -> int:
    rng = random.Random(args.seed)
    host, _, port = args.target.rpartition(":")
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", args.listen))
    server.listen(1)
    print(f"noisy: listening on {args.listen}, forwarding to {args.target}, "
          f"corrupting {args.corrupt:.1%} of bytes", file=sys.stderr, flush=True)
    client, _ = server.accept()
    upstream = socket.create_connection((host or "127.0.0.1", int(port)))
    for s in (client, upstream):
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    peer = {client: upstream, upstream: client}
    seen = flipped = 0
    try:
        while True:
            ready, _, _ = select.select([client, upstream], [], [])
            for s in ready:
                data = bytearray(s.recv(4096))
                if not data:
                    raise ConnectionError
                for i in range(len(data)):
                    if rng.random() < args.corrupt:
                        data[i] ^= 1 << rng.randrange(8)
                        flipped += 1
                seen += len(data)
                peer[s].sendall(bytes(data))
    except (ConnectionError, KeyboardInterrupt, OSError):
        pass
    print(f"noisy: {seen} bytes forwarded, {flipped} corrupted", file=sys.stderr)
    return 0


def _add_link_options(p: argparse.ArgumentParser) -> None:
    p.add_argument("transport", help="tcp:HOST:PORT, listen:PORT or serial:PORT@BAUD")
    p.add_argument("--window", type=int, default=8)
    p.add_argument("--slots", type=int, default=8)
    p.add_argument("--rto", type=int, default=50, help="retransmit timeout in ms")
    p.add_argument("--retries", type=int, default=10)
    p.add_argument("--timeout", type=float, default=10.0, help="seconds")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="framelink", description="framelink host tool")
    parser.add_argument("--version", action="version", version=f"framelink {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("monitor", help="decode a raw byte stream and print frames")
    p.add_argument("transport")
    p.set_defaults(func=cmd_monitor)

    p = sub.add_parser("send", help="send messages and wait until they are acknowledged")
    _add_link_options(p)
    p.add_argument("--type", type=lambda s: int(s, 0), default=0x01)
    p.add_argument("message", nargs="+")
    p.set_defaults(func=cmd_send)

    p = sub.add_parser("recv", help="print every frame received")
    _add_link_options(p)
    p.add_argument("--count", type=int, default=0, help="stop after N frames, 0 = never")
    p.add_argument("--text", action="store_true", help="print payloads as UTF-8")
    p.set_defaults(func=cmd_recv)

    p = sub.add_parser("bench", help="echo benchmark against fl_peer")
    _add_link_options(p)
    p.add_argument("--count", type=int, default=1000)
    p.add_argument("--size", type=int, default=16)
    p.add_argument("--corrupt", type=float, default=0.0, help="per-byte corruption rate, e.g. 0.05")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_bench, timeout=120.0)

    p = sub.add_parser("noisy", help="TCP proxy that flips bits in both directions")
    p.add_argument("--listen", type=int, required=True)
    p.add_argument("--target", required=True, help="HOST:PORT")
    p.add_argument("--corrupt", type=float, required=True)
    p.add_argument("--seed", type=int, default=1)
    p.set_defaults(func=cmd_noisy)
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
