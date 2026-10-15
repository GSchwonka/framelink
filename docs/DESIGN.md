# framelink design notes

This document explains why framelink is built the way it is. The wire format itself is defined in [SPEC.md](SPEC.md). Here the goal is to justify each choice, name the alternatives that were rejected, and be explicit about what the protocol does not protect against.

## Background

> **Author's note:** replace this block with the production story. It should say what the device and link were, what the symptom looked like in the field, which of the failure classes below caused it, and how it was found and fixed. Two or three paragraphs are enough. This is the part a reader remembers.

Hand-rolled serial protocols tend to fail in the same four ways:

1. **Desync.** The decoder trusts a corrupted length byte and consumes the next frame as payload.
2. **Collateral drops.** After a CRC failure the decoder throws away its whole buffer, including the start of a valid frame.
3. **Silent loss.** There is no acknowledgement, so nobody knows a frame was lost.
4. **Overrun.** There is no flow control, so a fast sender fills the receiver's buffers and frames vanish under load.

framelink addresses each of these with one mechanism, and each mechanism has a test that fails without it.

| Failure | Mechanism | Proving test |
|---------|-----------|--------------|
| Desync | Header CRC-8 | Corrupt LEN, then check the next frame is delivered promptly |
| Collateral drops | Rescan from offset 1 | Truncated frame followed by a valid frame |
| Silent loss | Sequence numbers, ACK/NAK, retransmit | Lossy loopback delivers all frames in order |
| Overrun | Credit-based flow control | Slow consumer shows zero `overflows` |

## Design constraints

- **C99, two files, no dependencies.** Integrating the library means copying two files.
- **Zero dynamic allocation.** All memory comes from the caller. Worst-case RAM use is known at compile time.
- **Runs on an 8 KB-RAM part.** Estimated footprint for a 4-frame window and a 4-frame receive queue at the maximum 255-byte payload:

  | Item | Bytes |
  |------|-------|
  | Transmit buffer, 4 x 262 | 1048 |
  | Receive queue, 4 x 257 | 1028 |
  | Decoder frame buffer | 262 |
  | Context and counters | about 100 |
  | **Total** | **about 2.4 KB** |

  The final figure will be measured with `arm-none-eabi-size` and recorded in [MEASUREMENTS.md](MEASUREMENTS.md). A build with a smaller `FL_MAX_PAYLOAD` shrinks every row except the context.

- **No threads, no timers, no clock.** The caller passes `now_ms` into `fl_poll`. All time comparisons use unsigned subtraction, so the 49.7-day wraparound of a 32-bit millisecond counter is harmless.
- **Transport-agnostic.** The library sees only `read_bytes` and `write_bytes`. UART, TCP and BLE differ only in the port code.

## Prior art

framelink was designed after reading the two closest open-source C libraries, MIN and TinyFrame. The notes below come from their source code, at MIN commit `40315e7` and TinyFrame commit `a29167a`. They describe the trade-offs each library made, not defects. Each one made sensible choices for its own goals.

### MIN 2.0

[MIN](https://github.com/min-protocol/min) is the closest relative. It has a C target library, a Python host library and an optional reliable transport.

- **Framing.** A frame starts with three `0xAA` bytes. Inside the frame, a `0x55` is stuffed after every two consecutive `0xAA` bytes, so a header can never appear inside a frame. Resync is therefore unambiguous. The cost is a data-dependent frame size, up to about 50% larger for payloads full of `0xAA`.
- **Integrity.** CRC-32, 4 bytes per frame. That is stronger than framelink's CRC-8 plus CRC16. framelink trades it for 1 byte less overhead and a cheaper per-byte cost on 8-bit and Cortex-M0 parts. See [the CRC section](#why-crc16-ccitt-instead-of-a-checksum).
- **Transport.** 8-bit sequence numbers, ACKs, and a sender window of up to 16 frames. The receiver accepts only the next expected sequence number and drops the rest. The target side never asks for a retransmission. On timeout, the sender resends only the oldest unacknowledged frame.
- **Flow control.** None from the receiver. Frames go to the application through a callback while the frame is being decoded. If the application cannot keep up, it must either block the decoder or drop data. The sender only knows about its own window and queue space.

**What framelink takes from MIN:** a C target with a matching Python host in the same repository, 8-bit sequence numbers, and in-order delivery with no reorder buffer.

**What framelink changes:** the receiver advertises free queue slots in every ACK, so the sender stops before the receiver overflows. Frames go into a caller-owned queue rather than straight to a callback, so the application consumes them at its own pace and that pace becomes the credit signal.

### TinyFrame

[TinyFrame](https://github.com/MightyPork/TinyFrame) is a flexible framing toolkit rather than a reliable link.

- **Framing.** An optional SOF byte, configurable widths for ID, LEN and TYPE, and an optional header checksum. framelink's header CRC follows the same idea. TinyFrame had it first.
- **Integrity.** Configurable: none, XOR, CRC-8, CRC-16, CRC-32 or a custom function.
- **Recovery.** On a header or body checksum failure, the parser resets to SOF hunting and drops what it has buffered. A valid frame that started inside the failed candidate is lost, unless its SOF byte arrives after the reset. framelink rescans the buffered bytes to keep that frame. This is the "collateral drops" failure from the background section.
- **Delivery.** No ACK, retransmission or flow control. Its ID listeners with timeouts let the application build request and response on top.
- **Memory.** `TF_Init` uses `malloc`. `TF_InitStatic` avoids it.

**What framelink takes from TinyFrame:** the header checksum, and a parser timeout driven by the caller rather than an internal timer.

### What neither library publishes

At the commits above, neither repository has a fuzz harness or a CI pipeline. Neither publishes CPU cost per byte on real hardware, or delivery numbers under injected corruption. These gaps are not criticisms. They are simply the gaps framelink exists to fill, which is why [MEASUREMENTS.md](MEASUREMENTS.md) is as long as it is.

## Framing without byte stuffing

HDLC-style stuffing and COBS both make the SYNC byte unambiguous, so a decoder can never mistake payload for a frame start. framelink uses neither, and accepts that `0x7E` may appear inside a frame.

**Why:**

- Stuffing makes frame size data-dependent. HDLC can double a frame in the worst case, which doubles the worst-case buffer on a RAM-constrained part.
- COBS has small, bounded overhead, but it needs the whole frame before encoding and is harder to decode incrementally.
- With a header CRC, a false SYNC inside the payload is rejected after 4 more bytes with probability 255/256. The remaining 1/256 case is caught by CRC16 and handled by rescan.

**Cost:** a rescan can re-feed up to 261 buffered bytes. In the worst case the decoder does more than one pass over some bytes. The fuzzer includes a throughput check so this cannot become a denial-of-service hole, and [MEASUREMENTS.md](MEASUREMENTS.md) reports the worst-case cycles per byte alongside the normal case.

COBS is a defensible alternative. The trade-off is recorded here so a reader can disagree on informed grounds.

## Why a separate header CRC

Without a header check, a corrupted LEN is only discovered when CRC16 fails, which is after LEN bytes have arrived. If LEN was corrupted to 250, the decoder swallows the next several valid frames as payload before it finds out. Rescan would eventually find them, but only after those 250 bytes arrived, which adds latency and forces retransmits.

The CRC-8 costs one byte per frame and catches the corrupted header after 5 bytes. It also makes NAK possible. If HCRC passed but CRC16 failed, the SEQ field can be trusted, so the receiver can ask for a retransmission immediately instead of waiting for a timeout.

**Detection properties** of the CRC-8 polynomial `0x07` over the 24-bit header:

- The polynomial is divisible by `x + 1`, so every error with an odd number of flipped bits is detected.
- Every burst of 8 bits or fewer is detected.
- Every 2-bit and 3-bit error is detected. There are only 2,300 such patterns in 24 bits, so `tests/` checks all of them exhaustively instead of relying on published tables.

## Why CRC16-CCITT instead of a checksum

A simple additive checksum is the most common choice in hand-rolled protocols, and it is weak in exactly the way serial links fail.

| Error pattern | 8-bit additive sum | 16-bit Fletcher | CRC16-CCITT |
|---------------|--------------------|-----------------|-------------|
| Any single-bit error | detected | detected | detected |
| Two bytes swapped | **missed** | detected | detected |
| Any 2-bit error in a max-size frame | often missed | mostly detected | detected |
| Any odd number of bit errors | not guaranteed | not guaranteed | detected |
| Any burst of 16 bits or fewer | not guaranteed | not guaranteed | detected |
| Random corruption, undetected rate | about 1 in 256 | about 1 in 65,000 | about 1 in 65,536 |

The polynomial `0x1021` includes the factor `x + 1` and gives a Hamming distance of 4 for messages up to 32,751 bits. The largest framelink frame is 2,096 bits, so every 1, 2 and 3-bit error in any frame is detected.

A table-driven implementation costs 512 bytes of flash. The bitwise version costs a few dozen bytes and roughly 8 times more cycles per byte. The default is bitwise, and `FL_CRC_TABLE` selects the table. Both costs are measured.

A 32-bit CRC would lower the undetected rate further, at 2 more bytes per frame. For the error rates of a real UART this is not worth it. For a very noisy link, see [failure modes](#failure-modes-framelink-does-not-protect-against).

## Why credit-based flow control instead of XON/XOFF

The scarce resource on a small receiver is not bytes per second. It is free slots in the receive queue. The flow control should talk about that resource directly.

**XON/XOFF** sends a single `0x13` byte to pause the sender and `0x11` to resume it.

- In a binary protocol those byte values occur in payloads, so they must be escaped. That brings back byte stuffing.
- The control bytes are not protected by any CRC. One flipped bit can create a spurious pause or erase a real one.
- A lost XOFF means overrun. A lost XON means deadlock, and nothing in XON/XOFF recovers from it.
- It is reactive. By the time XOFF reaches the sender, the UART FIFO, the DMA buffer and the sender's own queue already hold data in flight. The receiver needs headroom for all of it, and the right amount depends on baud rate and latency.

**Hardware RTS/CTS** fixes the reliability problems but needs extra wires. It does not exist over TCP or BLE, and USB-to-UART bridges implement it inconsistently.

**Credits** work as follows:

- The receiver puts its free queue slots into every ACK. The sender may have at most that many frames in flight past the last acknowledged one.
- It is proactive. The sender knows before it transmits whether there is room, so no frame is ever sent that will be dropped.
- Credit updates are frames, protected by both CRCs.
- Credits ride on ACKs that are already being sent, so the cost is one byte per ACK.
- It counts frames, which is what the receiver actually allocates.
- It works the same over UART, TCP and BLE.

**Deadlock.** If the receiver is full, the sender stops. When the application drains the queue, the receiver sends a CREDIT update, and that update can be lost. To recover, a blocked sender sends an empty CREDIT probe every `rto_ms`, which the receiver must answer. This is the same idea as the TCP persist timer. Probes do not count towards `max_retries`, because a live receiver that is full is not a dead link.

**Measured proof.** The throughput tests run a deliberately slow consumer twice: once with credits, once with credits forced to unlimited. The second run shows the overflow drops and retransmission waste that credits prevent. See [MEASUREMENTS.md](MEASUREMENTS.md).

## Why Go-Back-N instead of selective repeat

With selective repeat, the receiver keeps out-of-order frames and only the missing one is resent. With Go-Back-N, the receiver discards out-of-order frames and the sender resends everything from the gap.

framelink uses Go-Back-N because:

- The receiver needs no reorder buffer and no per-slot bitmap, which saves RAM and code.
- On a point-to-point serial link, windows are small, typically 4 to 8 frames, so the extra retransmission is cheap.
- The "one NAK per gap" rule in the spec prevents the retransmission storms Go-Back-N is known for.

The cost shows up on very lossy links, where each loss resends a whole window. The recovery measurements at high error rates quantify it. Selective repeat is a possible v2 option.

## Sequence wraparound

SEQ is 8 bits and wraps from 255 to 0. Comparisons use serial-number arithmetic:

```c
static inline int fl_seq_after(uint8_t a, uint8_t b) {
    uint8_t d = (uint8_t)(a - b);
    return d != 0 && d < 128;
}
```

For this to be unambiguous, the sender must never have more than 127 frames unacknowledged. Otherwise a retransmitted old frame and a new frame could share a SEQ, and the receiver could not tell them apart. The window is therefore limited to `1..127`, and `fl_init` rejects larger values.

Worked example with window 4:

| Step | Sender `snd_una` | Frames in flight | Receiver `rcv_nxt` | Note |
|------|------------------|------------------|--------------------|------|
| 1 | 253 | 253 254 255 0 | 253 | Window spans the wrap |
| 2 | 253 | 253 254 255 0 | 255 | 253 and 254 delivered |
| 3 | 255 | 255 0 | 255 | ACK 254 arrives. Frame 255 was corrupted |
| 4 | 255 | 255 0 | 255 | Frame 0 arrives, is "after" 255, out of order. One NAK for 255 |
| 5 | 255 | 255 0 1 2 | 3 | Sender resends from 255. All delivered in order |

8-bit sequence numbers were chosen over 16-bit because a 127-frame window is far more than a serial link needs, and every frame saves a byte. The wraparound test sends thousands of frames through the maximum window with random loss and checks strict ordering.

## Handshake and session IDs

A handshake has to answer one question: does the peer's sequence state match mine? Both sides start at SEQ 0 after a handshake, so a reset on one side must reset the other.

The naive rule, "reset whenever a HELLO arrives", has a race. A peer sends HELLO, waits one `rto_ms`, and sends it again because the HELLO_ACK has not arrived yet. If the second HELLO reaches the other side after the link is up and data has flowed, that side resets a perfectly good link. Sequence numbers then disagree and the link has to fail and recover.

framelink solves this with a one-byte session ID in every HELLO, chosen afresh each time a peer starts a link:

- A HELLO with a **new** session ID means the peer really restarted. Reset and reply.
- A HELLO with the **same** session ID is a retransmission. Reply, but change nothing.
- A HELLO_ACK must **echo** the receiver's own current session ID, so a late HELLO_ACK from an earlier attempt is ignored.

The ID only has to differ between consecutive sessions of the same peer, so one byte is enough. The application seeds it at boot, ideally from a random source. If a rebooted peer happens to reuse its last ID, the link still heals: the sequence numbers disagree, retries run out, both sides declare the link down, and the next handshake uses new IDs. That recovery path is covered by tests.

## Link reset and delivery guarantees

- Within one session, delivery is **exactly once and in order**.
- When `max_retries` is exceeded, the link goes down and the handshake restarts with fresh sequence numbers.
- Frames that were unacknowledged at that moment are reported to the application through the `FL_EV_LINK_DOWN` event, with their count, not silently resent. Some of them may already have been delivered, because the data frame arrived but its ACK was lost.
- Across a reset, delivery is therefore **at most once, with unknown status for in-flight frames**. The application decides whether to resend them. Applications that need exactly-once across resets should put an idempotency key in the payload.

This limit is fundamental. Solving it needs state that survives the reset, which contradicts the goal of a small, stateless-at-boot library.

## Failure modes framelink does not protect against

Being honest about limits is a feature. framelink does not protect against:

- **Undetected corruption.** Random corruption that beats both CRCs gets through at a rate of roughly 1 in 16 million corrupted frames. On a link that corrupts most frames, this can happen in practice. Put an end-to-end check in the payload if that matters.
- **A malicious peer.** There is no authentication. Anyone who can write to the stream can inject frames, lie about credits or force resets.
- **A peer that ignores credits.** framelink detects it through the `overflows` counter, but cannot prevent it.
- **Extreme error rates.** At 10% corrupted bytes, a frame with a 64-byte payload arrives intact about 0.06% of the time. No retransmission scheme fixes that. Smaller frames help, and the measurements show by how much.
- **Silent peer death on an idle link.** Without traffic, nothing notices. The application must send PING periodically if it needs to know.
- **Stale data after a long stall.** A frame stuck in a transport buffer for longer than 127 frames of traffic could alias a new SEQ. This cannot happen on a UART. Over TCP, the TCP layer already prevents it.
- **Bugs in the port code.** A UART driver that drops bytes under interrupt load shows up as corruption, which framelink recovers from, but the root cause is still a driver bug.

## Non-goals

These are out of scope for v1. Each would add code, RAM and attack surface to every user, including users who do not need it.

- **Encryption and authentication.** Use a secure transport, or a layer above framelink.
- **Compression.** Payloads on small links are usually already compact. Compression belongs to the application.
- **Payloads over 255 bytes and fragmentation.** A 16-bit LEN would double the header-length cost and the worst-case buffer.
- **Multiple channels, priorities or addressing.** Use TYPE to multiplex logical streams.
- **Selective repeat.** Possible in v2 if measurements show Go-Back-N is a bottleneck.
- **Dynamic allocation.** Never.
