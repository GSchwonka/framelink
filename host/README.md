# framelink for Python

The Python reference implementation of [framelink](https://github.com/GSchwonka/framelink), a reliable framed messaging protocol for UART, TCP and BLE. It speaks the same wire format as the C library, so a laptop can talk to an ESP32 or STM32 running framelink.

- Pure Python, no dependencies. `pyserial` is optional, for serial ports.
- Mirrors the C implementation function by function and is tested against the same vectors.
- Like the C library, it owns no clock and no thread. You call `poll()` with a millisecond timestamp.

## Install

```sh
pip install -e "host[serial]"     # from the repository root
```

## Example

```python
import time
from framelink import Busy, Config, Event, Link

def write(data: bytes) -> None:
    port.write(data)                      # any byte transport

def on_event(event: Event, arg: int) -> None:
    if event is Event.LINK_DOWN:
        print(f"{arg} frames were never acknowledged")

link = Link(write, Config(window=8, rx_slots=8, rto_ms=50, session_seed=42), on_event)

while True:
    link.poll(int(time.monotonic() * 1000))
    link.input(port.read(port.in_waiting or 1))

    frame = link.recv()                   # in order, CRC-checked, exactly once
    if frame is not None:
        print(frame.type, frame.payload)

    if link.connected:
        try:
            link.send(0x01, b"hello")
        except Busy:
            pass                          # window full or peer out of credits
```

## Command-line tool

Installing the package adds a `framelink` command. A transport is `tcp:HOST:PORT`, `listen:PORT` or `serial:PORT@BAUD`.

| Command | What it does |
|---------|--------------|
| `framelink monitor TRANSPORT` | Decode a raw byte stream and print every frame, with decoder statistics at the end |
| `framelink send TRANSPORT MSG...` | Connect, send each message as a frame, and wait until all are acknowledged |
| `framelink recv TRANSPORT [--text]` | Connect and print every frame received |
| `framelink bench TRANSPORT [--corrupt R]` | Echo benchmark against `fl_peer`, optionally corrupting a fraction R of bytes in both directions |
| `framelink noisy --listen P --target H:P --corrupt R` | TCP proxy that corrupts bytes in both directions, for testing any two endpoints |

The benchmark puts a frame counter and a CRC-32 inside every payload, checked outside framelink. Any corruption that slipped past framelink's own CRCs would show up as `undetected`.

## Tests

```sh
pip install -e "host[test]"
pytest host/tests
```

The codec tests read `tests/vectors/frames.json`, the same file the C tests use. The link tests mirror `tests/test_link.c`.

The interop tests run against the C library through `examples/tcp_peer`. Build it with `-DFL_BUILD_EXAMPLES=ON`, then set `FL_PEER` to its path. Without `FL_PEER` they are skipped.
