# framelink development plan

Target: v1.0.0 in about four weekends. The scope guard is strict: anything not listed here is v2.

## 1. Design decisions

These shape every file, so they are fixed before any code is written. Changing one means updating `docs/SPEC.md` and the test vectors together.

| # | Topic | Decision | Rationale |
|---|-------|----------|-----------|
| D1 | Frame layout | `SYNC(0x7E) LEN TYPE SEQ HCRC PAYLOAD CRC16` | 7 bytes overhead. The header CRC-8 rejects a corrupted `LEN` after 5 bytes instead of consuming up to 257 bytes of the next frame. |
| D2 | Resync strategy | On any CRC failure, rescan the buffered bytes starting from offset 1 | A valid frame that began inside a bad frame is never lost. |
| D3 | Length field | 1 byte, payload 0..255 | Fits the 8 KB RAM goal and keeps the decoder small. 16-bit length is a v2 non-goal. |
| D4 | Sequence numbers | 8-bit, serial-number arithmetic, window <= 127 | Ordering stays unambiguous across wraparound from 255 to 0. |
| D5 | Type namespace | User `0x00`-`0xEF`, reserved `0xF0`-`0xFF` | Reserved: ACK, NAK, CREDIT, HELLO/RESET, PING/PONG. |
| D6 | ACK contents | Cumulative sequence plus current credit count | Normal traffic never needs a standalone CREDIT frame. |
| D7 | Flow control | Credits = free slots in the receiver's queue. In flight <= `min(window, credits)` | Back-pressure is explicit and in-band. No XON/XOFF. |
| D8 | Time and threads | Library owns no timers or threads. Caller passes `now_ms` to `fl_poll` | Portable to bare metal, RTOS and host alike. |
| D9 | Retries | Runtime `rto_ms` and `max_retries`. Exhausting retries fires `on_link_down` | Failure is reported, never silent. |
| D10 | Memory | Zero allocation. Caller supplies tx and rx buffers | Usable on an 8 KB-RAM part. |
| D11 | CRC16 | CRC16-CCITT, table-less by default, optional 512-byte table via `FL_CRC_TABLE` | Flash vs speed trade-off is the user's choice and is documented. |
| D12 | Host tooling | Python only | A Go CLI adds a second toolchain for no gain within the time budget. |
| D13 | Unit tests | Unity, vendored | Matches the no-dependency theme. |
| D14 | Fuzzing | One harness buildable with libFuzzer and with AFL++ via stdin | Run under WSL2 or Linux with clang, since ASan on Windows is unreliable. |
| D15 | License | MIT | Standard for embedded libraries. |

## 2. Repository layout

```
framelink/
  framelink/          framelink.h, framelink.c
  ports/esp32/        ESP-IDF project: uart_example, tcp_example
  ports/stm32/        later: bare-metal Nucleo project, built in CI only
  host/framelink_py/  Python package + CLI
  tests/              Unity tests, CMake glue, vectors/*.json
  fuzz/               harness, corpus/, framelink.dict, run.sh
  bench/              measurement scripts, results/*.csv
  docs/                SPEC.md, DESIGN.md, MEASUREMENTS.md, PLAN.md
  .github/workflows/  ci.yml
  CMakeLists.txt  README.md  LICENSE  CONTRIBUTING.md  .gitignore
```

Inside the two library files there are three layers with distinct prefixes:

- **Codec** (`fl_codec_*`): CRCs, encoder and the decoder state machine. Callable on its own, which is what the fuzzer and the cycles-per-byte benchmark target.
- **Link** (`fl_link_*`): send window, retransmit timers, ACK/NAK, credits, HELLO handshake.
- **Glue** (`fl_*` public API): wires the callbacks to the codec and link layers.

## 3. Public API sketch

```c
typedef struct fl_config {
    uint8_t  *tx_buf;  size_t tx_buf_len;    /* window * max frame   */
    uint8_t  *rx_buf;  size_t rx_buf_len;    /* rx queue * max frame */
    uint8_t   window;                        /* 1..127               */
    uint16_t  rto_ms;
    uint8_t   max_retries;
    int  (*write_bytes)(void *user, const uint8_t *p, size_t n);
    int  (*read_bytes)(void *user, uint8_t *p, size_t n);
    void (*on_frame)(void *user, uint8_t type, const uint8_t *p, size_t n);
    void (*on_delivered)(void *user, uint8_t seq);
    void (*on_link_down)(void *user);
    void  *user;
} fl_config;

fl_err   fl_init(fl_ctx *ctx, const fl_config *cfg);
fl_err   fl_send(fl_ctx *ctx, uint8_t type, const uint8_t *p, size_t n); /* FL_EBUSY if window or credits exhausted */
void     fl_poll(fl_ctx *ctx, uint32_t now_ms);                          /* read, decode, ack, retransmit */
size_t   fl_rx_pending(const fl_ctx *ctx);
fl_stats fl_get_stats(const fl_ctx *ctx);  /* crc_errors, hdr_errors, resyncs, retransmits, dup_drops, overflows */
```

`fl_stats` is a first-class feature. Every published measurement is derived from it, which keeps the numbers reproducible.

## 4. Phases

### Weekend 1: codec, tests, fuzzing, CI

- [x] Write `docs/SPEC.md`: byte diagram, field semantics, CRC parameters, control frame payloads, test vectors.
- [x] Write `CONTRIBUTING.md` with build commands and the hard rules (C99, two files, no malloc, no dependencies).
- [x] Implement CRC-8 (header) and CRC16-CCITT, checked against published vectors.
- [x] Implement the encoder.
- [x] Implement the decoder states `HUNT → LEN → TYPE → SEQ → HCRC → PAYLOAD → CRC_LO → CRC_HI` with rescan on failure.
- [x] Unity tests:
  - [x] Round trip for payload sizes 0, 1, 254, 255.
  - [x] Each header byte corrupted in turn.
  - [x] Truncated frame followed by a valid frame: the valid one is delivered.
  - [x] `0x7E` inside payload and inside CRC bytes.
  - [x] Random garbage prefix before a valid frame.
  - [x] Two back-to-back frames split at every possible byte boundary.
  - [x] Exhaustive check that HCRC detects every 1, 2 and 3-bit header error (see DESIGN.md).
- [x] Shared JSON vectors in `tests/vectors/`, used later by the Python tests too.
- [x] Fuzz harness with seed corpus and dictionary. Run at least 1 million iterations. First CI run: 1.7 million decoder and 5.4 million round-trip executions in 60 seconds each, no crashes.
- [x] CI: gcc and clang, `-Wall -Wextra -Wpedantic -Wconversion -Werror`, ASan + UBSan test run, 60-second fuzz smoke run.

**Exit criteria:** CI green, decoder fuzzed with zero crashes, spec matches implementation.

### Weekend 2: link layer and Python host

- [x] Send window with a single Go-Back-N retransmit timer. Per-frame deadlines were replaced in the spec, see SPEC.md 5.3.
- [x] Cumulative ACK carrying credits. NAK when a frame with a known sequence fails its CRC16.
- [x] Duplicate detection on the receiver.
- [x] Credit accounting: credits released when the application consumes a frame, with zero-window probes.
- [x] HELLO handshake that resets sequences and advertises initial credits, with session IDs so a late HELLO cannot reset a live link.
- [x] `max_retries` exhaustion raises `FL_EV_LINK_DOWN` with the number of unacknowledged frames.
- [x] Unit tests for wraparound: 5,000 frames through a window of 127.
- [x] Unit tests over a simulated faulty wire: drops, corruption, duplicates from lost ACKs, cuts and peer restarts. Byte streams cannot reorder frames, so reordering is out of scope.
- [x] Link-layer fuzz harness where the fuzzer plays the peer.
- [x] Python package in `host/src/framelink`, mirroring the C state machine and tested against the shared JSON vectors.
- [x] Python CLI: `send`, `recv`, `monitor`, `bench`, `noisy`.
- [x] Interop test in CI: the C peer in `examples/tcp_peer` talks to the Python host over TCP.
- [x] Host recovery test at 1%, 5% and 10% corrupted bytes, run in CI by `bench/host_recovery.py`.

**Exit criteria:** Python↔C interop green in CI, recovery numbers captured on host.

### Weekend 3: ESP32 port

The only hardware available is one ESP32-WROOM-32D board, so v1 is measured on ESP32 alone. An ESP32 has three UARTs, so two of them wired together replace the planned ESP32-to-STM32 link.

- [x] ESP-IDF project with four apps: echo over USB serial, echo over Wi-Fi TCP, UART1-to-UART2 loopback, and a cycle-count benchmark.
- [x] Decoder benchmark with a worst-case input that maximises rescanning.
- [x] CI builds every app with ESP-IDF 5.5 and 6.1.
- [x] `bench/esp32_collect.py` reads results from the board, and `bench/host_recovery.py --device` runs recovery against it.
- [ ] Flash and run the benchmark, bitwise and table CRC. **Needs the board.**
- [ ] Wire GPIO25 to GPIO16 and GPIO17 to GPIO26, then run the loopback throughput tests. **Needs the board.**
- [ ] Run interop and recovery over USB serial and over Wi-Fi. **Needs the board.**
- [ ] Commit the raw results in `bench/results/` and fill in MEASUREMENTS.md and the README.
- [ ] Later, optional: STM32 bare-metal port compiled in CI, clearly marked as not measured on hardware.

**Exit criteria:** every ESP32 table in MEASUREMENTS.md has a real number and a raw result file.

### Weekend 4: measurements, design doc, release

- [ ] Throughput at 115200 and 921600 baud, with flow control on and with credits forced to unlimited. Record overflow drops in the second case, since that contrast is the flow-control argument.
- [ ] Hardware recovery test at 1%, 5% and 10% bit flips.
- [ ] Interop matrix: C↔Python over TCP, ESP32↔Python over USB serial and Wi-Fi, ESP32↔ESP32 over UART.
- [ ] Final long fuzz run. Record iteration count, crash count, clang version and date.
- [ ] Write `docs/MEASUREMENTS.md` with methodology per table and raw CSVs in `bench/results/`.
- [ ] Write `docs/DESIGN.md`:
  - [ ] Why credit-based flow control instead of XON/XOFF.
  - [ ] Why CRC16-CCITT instead of a checksum, with detection guarantees.
  - [ ] Why a separate header CRC.
  - [ ] Sequence wraparound and the window <= 127 rule.
  - [ ] Failure modes framelink does not protect against.
  - [ ] Non-goals and what v2 might add.
- [ ] Stretch: same-hardware comparison with MIN and TinyFrame, MEASUREMENTS.md section 7. Publish only if all three are measured like-for-like.
- [ ] Re-check the README comparison table against the latest MIN and TinyFrame commits, and update the commit hashes and date.
- [ ] Replace "TBD million iterations" in the README's one-sentence claim with the measured number.
- [ ] Fill the README tables, then tag `v1.0.0`.

**Exit criteria:** every README table has a real number with a linked methodology.

## 5. Documentation order

1. `docs/SPEC.md`: drafted. The code is written against it.
2. `CONTRIBUTING.md`: build commands and the hard constraints every change must respect.
3. `README.md`: done, measurement tables filled in weekend 4.
4. `docs/DESIGN.md`: drafted. Background story and measured numbers added in weekend 4.
5. `docs/MEASUREMENTS.md`: methodology drafted. Results filled in weekend 4.
6. `fuzz/README.md`, `ports/esp32/README.md`.

## 6. Non-goals for v1

- Encryption, authentication, compression.
- Payloads over 255 bytes, fragmentation.
- Multiple channels or addressing.
- Any dynamic allocation.
- A Go or Rust host implementation.

## 7. Open questions

- [x] Hardware: one ESP32-WROOM-32D dev board. No STM32 board, so STM32 is not measured in v1.
- [ ] Confirm the header CRC-8 byte (D1). This plan and the README assume it is accepted.
- [ ] Remove the empty outer git repo at `D:\Github Projects\FrameLink\.git`, or keep working only inside the nested clone.
- [x] Delete or gitignore `temp-prompt.txt` before the first push.
- [ ] Positioning. The README currently frames framelink as "a MIN-class protocol with explicit back-pressure", compared against MIN and TinyFrame. Two alternatives were considered:
  - Contribute credit-based flow control and a fuzz harness upstream to MIN instead. That puts the work in front of MIN's existing users and shows work in someone else's codebase, at the cost of control over scope and timing. Credits change MIN's ACK payload, so this would need the maintainers to accept a wire-format change.
  - Present framelink as a teaching reference: "the smallest complete, correct, documented implementation of framing, ARQ and flow control", with DESIGN.md as the main product. The current docs already support this angle, so it can be added later without rework.
