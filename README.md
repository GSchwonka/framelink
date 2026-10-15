# framelink

[![CI](https://github.com/GSchwonka/framelink/actions/workflows/ci.yml/badge.svg)](https://github.com/GSchwonka/framelink/actions/workflows/ci.yml)
![C99](https://img.shields.io/badge/C-99-blue)
![No malloc](https://img.shields.io/badge/heap-zero%20allocations-brightgreen)
![License: MIT](https://img.shields.io/badge/license-MIT-lightgrey)

**A dependency-free C99 library for reliable framed messaging over unreliable byte streams (UART, TCP, BLE), with CRC16-CCITT integrity, acknowledgements, credit-based back-pressure and a fuzz-tested decoder.**

In one sentence: **a MIN-class protocol with explicit back-pressure, fuzzed to TBD million iterations, benchmarked on ESP32 and STM32.** Each part of that claim is checkable. The fuzz harness is in `fuzz/`, the benchmarks are in `bench/`, and the method behind every number is in [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md).

> **Status:** in development. See [docs/PLAN.md](docs/PLAN.md) for the roadmap. Numbers in the tables below are filled in only after they are measured.

---

## Why

Every embedded team ends up writing this protocol, and most of them get the decoder wrong:

- **Silent drops.** A corrupted byte is discarded along with the valid frame right after it.
- **Desync.** One bad length byte and the decoder spends the next 200 bytes reading garbage.
- **No flow control.** The sender outruns the MCU, receive buffers overflow under load, and frames vanish without any error.

framelink is the library that prevents that class of bug. It is honest about which failures it protects against and which it does not.

## Features

- **Compact frame format** with a sync byte, length, type, sequence number, header CRC-8, payload and CRC16-CCITT. Full byte-level spec in [docs/SPEC.md](docs/SPEC.md).
- **Resynchronising decoder.** A state machine that recovers after garbage bytes or truncated frames without losing the next valid frame.
- **ACK/NAK with sequence numbers** and a configurable retransmit window.
- **Credit-based flow control.** The receiver advertises how many frames it can accept, and the sender never exceeds it.
- **Zero dynamic allocation.** The caller provides static buffers. Usable on an 8 KB-RAM part.
- **Transport-agnostic.** The core only sees `read_bytes` / `write_bytes` callbacks.
- **No threads, no timers.** You call `fl_poll(ctx, now_ms)` from your loop or task.
- **Two files.** Drop `framelink.h` and `framelink.c` into any C99 project.

## How it compares

[MIN](https://github.com/min-protocol/min) and [TinyFrame](https://github.com/MightyPork/TinyFrame) are the two open-source C libraries closest to framelink. Both are established, well-designed and MIT-licensed. framelink differs on the parts they leave out: back-pressure from the receiver, a fuzz harness, and published measurements.

| | framelink | MIN 2.0 | TinyFrame |
|---|---|---|---|
| Frame sync | `0x7E` + header CRC-8 | 3 x `0xAA` header + byte stuffing | Optional SOF byte + header checksum |
| Recovery after a bad frame | Rescans buffered bytes, so a frame that starts inside the bad one is kept | Waits for the next header. Stuffing makes it unambiguous | Resets to SOF hunting. Buffered bytes are dropped |
| Integrity check | CRC-8 header + CRC16-CCITT | CRC-32 | Configurable: none, XOR, CRC-8, CRC-16, CRC-32 or custom |
| Max payload | 255 bytes | 255 bytes | Configurable, up to 32-bit length |
| Reliable delivery | Sequence numbers, ACK, NAK, Go-Back-N retransmit | Sequence numbers, ACK, retransmit, window up to 16 | None. Request and response are matched by ID |
| **Flow control** | **Credit-based. The receiver advertises free slots** | None. The receiver must keep up, and out-of-sequence frames are dropped | None |
| Dynamic allocation | Never | None | Optional. `TF_InitStatic` avoids it |
| Host implementation | Python, same repo, tested in CI against C | Python, same repo | Python, Rust and JavaScript ports in separate repos |
| **Fuzz harness** | **Yes, `fuzz/`** | No | No |
| **CI** | **Yes** | No | No |
| **Published CPU cost per byte** | **ESP32 and STM32** | No | No |
| **Published recovery under injected errors** | **1%, 5%, 10%** | No | No |
| License | MIT | MIT | MIT |

MIN and TinyFrame were checked against their source at commits `40315e7` and `a29167a`, as of October 2026. The framelink column describes the v1 target. Rows in bold are verified as each phase of [docs/PLAN.md](docs/PLAN.md) lands. If anything here is wrong about either library, please open an issue.

**When to pick something else:**

- **MIN** if you want a stronger CRC-32 and a longer track record.
- **TinyFrame** if you want configurable field widths, listeners and query/response, and you do not need delivery guarantees.
- **framelink** if the receiver can be slower than the sender, or if you need to show evidence that the decoder survives hostile input.

The design trade-offs against both libraries are discussed in [docs/DESIGN.md](docs/DESIGN.md#prior-art).

## Frame format

```
 0        1        2        3        4        5 .. 5+N-1      5+N      6+N
+--------+--------+--------+--------+--------+---------------+--------+--------+
|  SYNC  |  LEN   |  TYPE  |  SEQ   |  HCRC  |   PAYLOAD     | CRC_LO | CRC_HI |
|  0x7E  |  N     |        |        |  CRC-8 |  0..255 bytes |   CRC16-CCITT   |
+--------+--------+--------+--------+--------+---------------+--------+--------+
```

- **HCRC** covers `LEN | TYPE | SEQ`. A corrupted header is rejected after 5 bytes instead of swallowing the following frame.
- **CRC16-CCITT** covers everything from `LEN` through the end of the payload.
- Overhead is 7 bytes per frame. Types `0xF0`-`0xFF` are reserved for control frames (ACK, NAK, CREDIT, HELLO, PING).

## Quick example

```c
#include "framelink.h"

static uint8_t tx_buf[FL_TX_BUF_SIZE(4)];   /* 4-frame send window  */
static uint8_t rx_buf[FL_RX_BUF_SIZE(4)];   /* 4-frame receive queue */
static fl_ctx  link;

static void on_frame(void *user, uint8_t type, const uint8_t *p, size_t n) {
    /* handle an in-order, CRC-checked, de-duplicated frame */
}

void app_init(void) {
    fl_config cfg = {
        .tx_buf = tx_buf, .tx_buf_len = sizeof tx_buf,
        .rx_buf = rx_buf, .rx_buf_len = sizeof rx_buf,
        .window = 4, .rto_ms = 50, .max_retries = 5,
        .write_bytes = uart_write, .read_bytes = uart_read,
        .on_frame = on_frame,
    };
    fl_init(&link, &cfg);
}

void app_loop(void) {
    fl_poll(&link, millis());
    if (fl_send(&link, 0x01, sensor_data, sizeof sensor_data) == FL_EBUSY) {
        /* window full or receiver out of credits: try again later */
    }
}
```

The API is still being finalised. The authoritative version will be in `framelink/framelink.h`.

## Repository layout

| Path | Contents |
|------|----------|
| `framelink/` | Core C99 library: `framelink.h`, `framelink.c` |
| `ports/esp32/` | ESP-IDF + FreeRTOS examples over UART and TCP |
| `ports/stm32/` | Bare-metal Nucleo example over USART + DMA |
| `host/` | Python reference implementation and CLI (`send`, `recv`, `monitor`, `bench`) |
| `tests/` | Unity unit tests and shared JSON test vectors, run on host in CI |
| `fuzz/` | libFuzzer / AFL++ harness for the decoder, with corpus |
| `bench/` | Measurement scripts and raw result CSVs |
| `docs/` | `SPEC.md`, `DESIGN.md`, `MEASUREMENTS.md`, `PLAN.md` |

## Building and testing

```sh
cmake -B build -DFL_SANITIZE=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Fuzzing (Linux or WSL2, clang):

```sh
cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DFL_FUZZ=ON
cmake --build build-fuzz
./build-fuzz/fuzz_decoder fuzz/corpus -max_total_time=600
```

## Measurements

Methodology for every table is in [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md).

**Decoder cost**

| Target | Clock | Cycles/byte |
|--------|-------|-------------|
| ESP32 | TBD | TBD |
| STM32 (Nucleo TBD) | TBD | TBD |

**UART throughput**

| Baud | Flow control | Payload throughput | Frames lost |
|------|--------------|--------------------|-------------|
| 115200 | on | TBD | TBD |
| 115200 | off | TBD | TBD |
| 921600 | on | TBD | TBD |
| 921600 | off | TBD | TBD |

**Recovery under injected corruption** (share of bytes with one bit flipped, 16-byte payloads)

| Corrupted bytes | Frames sent | Delivered | Retransmits | Link-down events |
|---------------|-------------|-----------|-------------|------------------|
| 1% | TBD | TBD | TBD | TBD |
| 5% | TBD | TBD | TBD | TBD |
| 10% | TBD | TBD | TBD | TBD |

**Interop matrix**

| | Python host | ESP32 | STM32 |
|---|---|---|---|
| **Python host** | ✅ CI | TBD | TBD |
| **ESP32** | TBD | — | TBD |
| **STM32** | TBD | TBD | — |

**Fuzzing:** TBD million iterations, TBD crashes. Toolchain and date will be recorded here.

## Non-goals for v1

- Encryption or authentication. Run framelink under a secure transport if you need it.
- Compression.
- Payloads over 255 bytes or fragmentation.
- Multiplexed channels or multi-drop addressing.
- Dynamic memory allocation of any kind.

The reasoning behind these choices is in [docs/DESIGN.md](docs/DESIGN.md).

## License

MIT
