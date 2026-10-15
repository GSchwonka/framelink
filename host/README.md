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

## Tests

```sh
pip install -e "host[test]"
pytest host/tests
```

The codec tests read `tests/vectors/frames.json`, the same file the C tests use. The link tests mirror `tests/test_link.c`.
