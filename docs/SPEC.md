# framelink wire protocol, version 1

This document is normative. The C library, the Python host implementation and the test vectors in `tests/vectors/` must all agree with it. If code and spec disagree, the spec wins and the code is the bug.

The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

## 1. Scope

framelink carries discrete messages between two peers over a single ordered byte stream that may corrupt, drop or insert bytes. Typical transports are UART, TCP and BLE serial characteristics.

The protocol provides:

- Frame delimitation and resynchronisation after corruption.
- Error detection on the header and on the whole frame.
- In-order, exactly-once delivery of data frames, using sequence numbers, acknowledgements and retransmission.
- Credit-based flow control, so a sender never overruns the receiver's queue.

It does not provide confidentiality, authentication, compression, fragmentation or addressing. See [DESIGN.md](DESIGN.md#non-goals).

## 2. Frame format

```
 offset  0        1        2        3        4        5 ..... 5+N-1   5+N      6+N
        +--------+--------+--------+--------+--------+---------------+--------+--------+
        |  SYNC  |  LEN   |  TYPE  |  SEQ   |  HCRC  |    PAYLOAD    | CRC_LO | CRC_HI |
        +--------+--------+--------+--------+--------+---------------+--------+--------+
        |  0x7E  |   N    |        |        | CRC-8  |  N bytes      |   CRC16-CCITT   |
        +--------+--------+--------+--------+--------+---------------+--------+--------+
                 |<---- HCRC input ------->|
                 |<-------------------- CRC16 input ------------------->|
```

| Field | Size | Description |
|-------|------|-------------|
| SYNC | 1 | Always `0x7E`. Marks a candidate frame start. |
| LEN | 1 | Payload length N, 0..255. Does not include header or CRC. |
| TYPE | 1 | Message type. See section 4. |
| SEQ | 1 | Sequence number for data frames. MUST be `0x00` on control frames and is ignored on receipt. |
| HCRC | 1 | CRC-8 over `LEN TYPE SEQ`. |
| PAYLOAD | N | Opaque application bytes, or a control payload defined in section 5. |
| CRC | 2 | CRC16-CCITT over `LEN TYPE SEQ HCRC PAYLOAD`, transmitted low byte first. |

- Frame overhead is 7 bytes. The largest frame is 262 bytes.
- The protocol uses no byte stuffing. `0x7E` MAY appear anywhere after the SYNC byte, including inside HCRC, PAYLOAD and CRC.
- Multi-byte fields are little-endian. The CRC is the only multi-byte field in version 1.

## 3. Checksums

Both CRCs are computed bitwise, MSB first, with no input or output reflection.

| Name | Width | Polynomial | Init | Reflect in/out | XOR out | Check value of `"123456789"` | Catalogue name |
|------|-------|------------|------|----------------|---------|------------------------------|----------------|
| HCRC | 8 | `0x07` | `0x00` | no / no | `0x00` | `0xF4` | CRC-8/SMBUS |
| CRC | 16 | `0x1021` | `0xFFFF` | no / no | `0x0000` | `0x29B1` | CRC-16/CCITT-FALSE |

Reference implementation, for clarity rather than speed:

```c
uint8_t fl_crc8(const uint8_t *p, size_t n) {
    uint8_t c = 0x00;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

uint16_t fl_crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}
```

## 4. Frame types

| TYPE | Name | Class | Payload |
|------|------|-------|---------|
| `0x00`-`0xEF` | user | data | Application defined, 0..255 bytes |
| `0xF0` | ACK | control | `ack_seq(1) credits(1)` |
| `0xF1` | NAK | control | `nak_seq(1) credits(1)` |
| `0xF2` | CREDIT | control | empty (probe) or `ack_seq(1) credits(1)` (update) |
| `0xF3` | HELLO | control | `version(1) window(1) credits(1) flags(1)` |
| `0xF4` | HELLO_ACK | control | `version(1) window(1) credits(1) flags(1)` |
| `0xF5` | PING | control | 0..8 bytes, echoed |
| `0xF6` | PONG | control | copy of the PING payload |
| `0xF7`-`0xFF` | reserved | control | A receiver MUST discard them and increment `unknown_type` |

- **Data frames** are sequenced, acknowledged, retransmitted and consume one credit each.
- **Control frames** are not sequenced, not acknowledged and do not consume credits. Losing one is always recovered by a timer, as described below.

## 5. Link behaviour

### 5.1 Sequence numbers

- SEQ is an 8-bit counter that wraps from `0xFF` to `0x00`.
- Comparison uses serial-number arithmetic: `a` is after `b` when `(uint8_t)(a - b)` is in `1..127`.
- The send window MUST be in `1..127`. This keeps "new" and "old" unambiguous across wraparound. See [DESIGN.md](DESIGN.md#sequence-wraparound).
- After a successful handshake, the first data frame in each direction has SEQ `0x00`.

### 5.2 Handshake

1. On start-up, or after a link-down event, a peer sends HELLO and repeats it every `rto_ms` until it receives HELLO or HELLO_ACK.
2. A peer that receives HELLO MUST reset its sequence state in both directions, then reply with HELLO_ACK.
3. `version` is `0x01`. A peer receiving a different version MUST NOT send data frames and SHOULD report the mismatch to the application.
4. `window` is the sender's window size. `credits` is the receiver's initial free queue slots. `flags` is reserved and MUST be `0x00`.
5. Before the handshake completes, a peer MUST NOT send data frames and MUST discard any data frames it receives.

### 5.3 Sending data

The sender keeps `snd_una`, the oldest unacknowledged SEQ, and `snd_nxt`, the next SEQ to assign.

A new data frame MAY be sent only when both conditions hold:

```
(uint8_t)(snd_nxt - snd_una) < window
(uint8_t)(snd_nxt - snd_una) < peer_credits
```

`peer_credits` is the credit count from the most recent ACK, NAK or CREDIT update. Credits are counted from `snd_una`, so a stale ACK can never grant more room than the receiver actually has.

Each unacknowledged frame has a retransmit deadline of `now + rto_ms`. When the oldest deadline expires, the sender retransmits every unacknowledged frame from `snd_una` in order, which is Go-Back-N. Each retransmission of `snd_una` increments its retry counter. When the counter exceeds `max_retries`, the link is declared down, and the handshake restarts.

### 5.4 Receiving data

The receiver keeps `rcv_nxt`, the next SEQ it expects.

| Received SEQ | Queue has space | Action |
|--------------|-----------------|--------|
| `== rcv_nxt` | yes | Enqueue, advance `rcv_nxt`, send ACK |
| `== rcv_nxt` | no | Discard, increment `overflows`, send ACK with credits 0 |
| before `rcv_nxt`, within 127 | any | Duplicate. Discard, increment `dup_drops`, send ACK |
| after `rcv_nxt` | any | Out of order. Discard, send NAK for `rcv_nxt` |

- ACK carries `ack_seq = rcv_nxt - 1`, the last SEQ delivered in order, and `credits`, the free slots in the receive queue. Before any data is received, `ack_seq` is `0xFF`.
- When a data frame passes HCRC but fails CRC16, the header is trusted. The receiver SHOULD send NAK for `rcv_nxt` immediately, which avoids waiting for a timeout.
- The receiver MUST send at most one NAK per value of `rcv_nxt`. Later out-of-order frames for the same gap are discarded silently. Without this rule, every frame still in flight triggers another full Go-Back-N retransmission.
- The receiver MAY coalesce ACKs, but MUST send one within `rto_ms / 2` of accepting a frame.

### 5.5 ACK and NAK processing

- On ACK or NAK, the sender releases every frame from `snd_una` through `ack_seq`, sets `snd_una = ack_seq + 1`, and updates `peer_credits`.
- For NAK, `ack_seq` is `nak_seq - 1`. After releasing, the sender retransmits from `nak_seq` immediately.
- ACKs whose `ack_seq` is before `snd_una - 1` are stale. The sender updates nothing except that it MAY use the credits if they are larger.

### 5.6 Credit updates and zero-window probing

- When the application consumes frames and the receive queue goes from full to non-full, the receiver SHOULD send a CREDIT update with the current `ack_seq` and `credits`.
- That update can be lost. A sender with pending data and `peer_credits == 0` MUST send an empty CREDIT probe every `rto_ms`. The receiver MUST answer a probe with a CREDIT update.
- Probes do not count towards `max_retries`. A receiver that is alive but full is not a dead link.

### 5.7 Keepalive

A peer MAY send PING at any time. The receiving peer MUST answer with PONG carrying the same payload. PING is how an idle link detects a dead peer.

## 6. Decoder

The decoder is a byte-at-a-time state machine that buffers at most one frame, 262 bytes.

```
                 +-----------------------------------------------+
                 |                  rescan(buf+1)                |
                 v                                               |
 byte -> [HUNT] --0x7E--> [LEN] -> [TYPE] -> [SEQ] -> [HCRC] ----+ HCRC bad
            ^                                            |
            |                                         HCRC ok
            |                                            v
            |                    [CRC_HI] <- [CRC_LO] <- [PAYLOAD] (N bytes, skipped if N = 0)
            |                        |
            |       CRC ok: emit frame, go to HUNT
            +---------------------- CRC bad: rescan(buf+1)
```

Rules:

1. In HUNT, every byte other than `0x7E` is discarded and counted in `hunt_bytes`.
2. On HCRC failure, increment `hdr_errors`. On CRC16 failure, increment `crc_errors`.
3. On either failure, the decoder MUST NOT discard the buffered bytes. It returns to HUNT and re-feeds the buffered bytes starting at offset 1, which is the rescan. Any valid frame that started inside the failed candidate is therefore still found.
4. Rescanning MUST NOT recurse. Implementations use a single loop over the buffer.
5. If `frame_timeout_ms` is non-zero and no byte arrives for that long while mid-frame, the decoder rescans immediately. This bounds the latency of recovering from a truncated frame with a large corrupted LEN.
6. The decoder MUST NOT read or write outside its buffer for any input. This property is what `fuzz/` tests.

## 7. Test vectors

All bytes are hex. These vectors live in `tests/vectors/frames.json` and are used by both the C and Python test suites.

| Description | TYPE | SEQ | Payload | Encoded frame |
|-------------|------|-----|---------|---------------|
| Empty data frame | `01` | `00` | none | `7E 00 01 00 15 64 F1` |
| Short data frame | `01` | `05` | `"hello"` | `7E 05 01 05 CE 68 65 6C 6C 6F 3B AC` |
| SYNC bytes inside payload | `10` | `FF` | `7E 7E 00` | `7E 03 10 FF 19 7E 7E 00 E8 69` |
| ACK, ack_seq 5, credits 4 | `F0` | `00` | `05 04` | `7E 02 F0 00 C2 05 04 E0 4D` |
| HELLO, v1, window 8, credits 8 | `F3` | `00` | `01 08 08 00` | `7E 04 F3 00 80 01 08 08 00 25 BD` |

## 8. Statistics

Every conforming implementation exposes these counters. The names are normative so that measurements from different implementations can be compared.

| Counter | Incremented when |
|---------|------------------|
| `rx_frames` | A frame passes both CRCs |
| `tx_frames` | A frame is written, including retransmissions |
| `hunt_bytes` | A byte is discarded while hunting for SYNC |
| `hdr_errors` | HCRC fails |
| `crc_errors` | CRC16 fails |
| `resyncs` | A rescan finds a valid frame inside a failed candidate |
| `retransmits` | A data frame is sent again |
| `dup_drops` | A duplicate data frame is discarded |
| `overflows` | An in-sequence data frame is discarded because the queue is full |
| `unknown_type` | A reserved TYPE is received |
| `link_down` | `max_retries` is exceeded |

With correct flow control, `overflows` stays at zero. A non-zero value means the peer ignored credits.
