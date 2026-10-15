/*
 * Fuzz harness for the whole link layer.
 *
 * The input is a small program. Each opcode byte is one of:
 *   00xxxxxx  feed the next x+1 input bytes to the link as received data
 *   01xxxxxx  advance the clock by 3*x ms and poll
 *   10xxxxxx  send a data frame of type x with up to 16 following bytes
 *   11xxxxxx  receive one frame (x selects the buffer size)
 *
 * The peer is the fuzzer itself, so it can forge any HELLO, ACK, NAK or
 * CREDIT, in any order and with any timing.
 *
 * Invariants checked after every step:
 *   - no memory error or undefined behaviour (ASan and UBSan)
 *   - frames in flight never exceed the window
 *   - the receive queue never exceeds its slots
 *   - every frame written by the link is a valid frame
 *   - received frames never exceed the buffer they are copied into
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#include <stdint.h>
#include <string.h>

#include "framelink.h"
#include "fuzz_check.h"

#define WINDOW 4u
#define SLOTS 3u

static fl_decoder check_dec;
static unsigned written_frames;

static void on_written_frame(void *user, const fl_frame *f)
{
    (void)user;
    (void)f;
    written_frames++;
}

/* Everything the link writes must decode as exactly whole, valid frames. */
static int on_write(void *user, const uint8_t *data, size_t len)
{
    unsigned before = written_frames;
    (void)user;
    FUZZ_CHECK(len >= FL_OVERHEAD && len <= FL_MAX_FRAME);
    fl_decoder_feed(&check_dec, data, len, on_written_frame, NULL);
    FUZZ_CHECK(written_frames == before + 1);
    FUZZ_CHECK(!fl_decoder_busy(&check_dec));
    return (int)len;
}

static void on_event(void *user, fl_event ev, uint8_t arg)
{
    (void)user;
    (void)arg;
    FUZZ_CHECK(ev >= FL_EV_CONNECTED && ev <= FL_EV_VERSION_MISMATCH);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    static fl_link link;
    static uint8_t tx[FL_TX_BUF_SIZE(WINDOW)];
    static uint8_t rx[FL_RX_BUF_SIZE(SLOTS)];
    fl_config cfg;
    uint32_t now = 0;
    size_t pos = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.tx_buf = tx;
    cfg.tx_buf_len = sizeof tx;
    cfg.rx_buf = rx;
    cfg.rx_buf_len = sizeof rx;
    cfg.window = WINDOW;
    cfg.max_retries = 3;
    cfg.rto_ms = 10;
    cfg.session_seed = 0x42;
    cfg.write_bytes = on_write;
    cfg.on_event = on_event;
    FUZZ_CHECK(fl_init(&link, &cfg) == FL_OK);
    fl_decoder_init(&check_dec);
    written_frames = 0;

    while (pos < size) {
        uint8_t op = data[pos++];
        uint8_t arg = op & 0x3Fu;
        switch (op >> 6) {
        case 0: {
            size_t n = (size_t)arg + 1u;
            if (n > size - pos) {
                n = size - pos;
            }
            fl_input(&link, data + pos, n);
            pos += n;
            break;
        }
        case 1:
            now += 3u * arg;
            fl_poll(&link, now);
            break;
        case 2: {
            size_t n = size - pos < 16u ? size - pos : 16u;
            fl_err err = fl_send(&link, arg, data + pos, n, NULL);
            FUZZ_CHECK(err == FL_OK || err == FL_EBUSY || err == FL_ENOTCONN);
            pos += n;
            break;
        }
        default: {
            uint8_t buf[FL_MAX_PAYLOAD + 1];
            size_t cap = (size_t)arg * 4u;
            size_t len = 0;
            uint8_t type = 0;
            fl_err err;
            if (cap > FL_MAX_PAYLOAD) {
                cap = FL_MAX_PAYLOAD;
            }
            err = fl_recv(&link, &type, buf, cap, &len);
            FUZZ_CHECK(err == FL_OK || err == FL_EAGAIN || err == FL_ENOSPC);
            if (err == FL_OK) {
                FUZZ_CHECK(len <= cap);
                FUZZ_CHECK(type <= FL_TYPE_USER_MAX);
            }
            break;
        }
        }
        FUZZ_CHECK(fl_tx_inflight(&link) <= WINDOW);
        FUZZ_CHECK(fl_rx_pending(&link) <= SLOTS);
    }
    return 0;
}
