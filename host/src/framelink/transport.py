"""Byte transports for the framelink host: TCP, serial, and a noisy wrapper.

A transport has three methods: write(bytes), read(timeout) -> bytes, and
close(). read() returns b"" when nothing arrived within the timeout and
raises ConnectionError when the other side has closed.

SPDX-License-Identifier: MIT
Copyright (c) 2026 Guilherme Schwonka
"""
from __future__ import annotations

import random
import select
import socket
from typing import Optional


class TcpTransport:
    def __init__(self, sock: socket.socket) -> None:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock = sock

    @classmethod
    def connect(cls, host: str, port: int, timeout: float = 5.0) -> "TcpTransport":
        return cls(socket.create_connection((host, port), timeout=timeout))

    @classmethod
    def listen(cls, port: int, host: str = "0.0.0.0") -> "TcpTransport":
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((host, port))
        server.listen(1)
        try:
            conn, _ = server.accept()
        finally:
            server.close()
        return cls(conn)

    def write(self, data: bytes) -> None:
        self.sock.sendall(data)

    def read(self, timeout: float = 0.0) -> bytes:
        ready, _, _ = select.select([self.sock], [], [], timeout)
        if not ready:
            return b""
        data = self.sock.recv(4096)
        if not data:
            raise ConnectionError("connection closed by peer")
        return data

    def close(self) -> None:
        self.sock.close()


class SerialTransport:
    def __init__(self, port: str, baud: int = 115200) -> None:
        try:
            import serial  # type: ignore
        except ImportError as exc:  # pragma: no cover - depends on the environment
            raise RuntimeError("serial ports need pyserial: pip install 'framelink[serial]'") from exc
        self.port = serial.Serial(port, baud, timeout=0)

    def write(self, data: bytes) -> None:
        self.port.write(data)

    def read(self, timeout: float = 0.0) -> bytes:
        self.port.timeout = timeout
        first = self.port.read(1)
        if not first:
            return b""
        return first + self.port.read(self.port.in_waiting)

    def close(self) -> None:
        self.port.close()


class NoisyTransport:
    """Wrap a transport and flip one random bit in each byte with probability
    `rate`, independently in both directions. This is the corruption model of
    docs/MEASUREMENTS.md section 4."""

    def __init__(self, inner, rate: float, seed: Optional[int] = None) -> None:
        if not 0.0 <= rate < 1.0:
            raise ValueError("rate must be in [0, 1)")
        self.inner = inner
        self.rate = rate
        self.rng = random.Random(seed)
        self.bytes_seen = 0
        self.bytes_corrupted = 0

    def _corrupt(self, data: bytes) -> bytes:
        self.bytes_seen += len(data)
        if self.rate == 0.0:
            return data
        out = bytearray(data)
        for i in range(len(out)):
            if self.rng.random() < self.rate:
                out[i] ^= 1 << self.rng.randrange(8)
                self.bytes_corrupted += 1
        return bytes(out)

    def write(self, data: bytes) -> None:
        self.inner.write(self._corrupt(data))

    def read(self, timeout: float = 0.0) -> bytes:
        return self._corrupt(self.inner.read(timeout))

    def close(self) -> None:
        self.inner.close()


def open_transport(spec: str):
    """Open a transport from a short description.

    tcp:HOST:PORT      connect to a TCP server
    listen:PORT        wait for one TCP connection
    serial:PORT@BAUD   open a serial port, for example serial:COM3@921600
    """
    kind, _, rest = spec.partition(":")
    if kind == "tcp":
        host, _, port = rest.rpartition(":")
        return TcpTransport.connect(host or "127.0.0.1", int(port))
    if kind == "listen":
        return TcpTransport.listen(int(rest))
    if kind == "serial":
        port, _, baud = rest.partition("@")
        return SerialTransport(port, int(baud or 115200))
    raise ValueError(f"unknown transport '{spec}', expected tcp:, listen: or serial:")
