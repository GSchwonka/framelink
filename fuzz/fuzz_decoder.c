/*
 * Fuzz harness for the framelink decoder.
 *
 * The input is treated as a raw byte stream from an untrusted peer. The first
 * byte selects how the stream is split into feed calls and how often the
 * inter-byte timeout fires, so the fuzzer also explores chunking.
 *
 * Invariants checked:
 *   - no memory error or undefined behaviour (ASan and UBSan)
 *   - every delivered frame has a length within FL_MAX_PAYLOAD
 *   - every delivered frame re-encodes and decodes back to itself
 *   - the frame counter matches the number of callbacks
 *   - after a final timeout the decoder holds no partial frame
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#include <stdint.h>
#include <string.h>

#include "framelink.h"
#include "fuzz_check.h"

typedef struct ctx {
    uint32_t frames;
} ctx;

typedef struct echo {
    int seen;
    fl_frame frame;
    uint8_t payload[FL_MAX_PAYLOAD + 1];
} echo;

static void on_echo(void *user, const fl_frame *f)
{
    echo *e = (echo *)user;
    e->seen++;
    e->frame = *f;
    if (f->len > 0) {
        memcpy(e->payload, f->payload, f->len);
    }
}

static void on_frame(void *user, const fl_frame *f)
{
    ctx *c = (ctx *)user;
    uint8_t buf[FL_MAX_FRAME];
    size_t n = 0;
    fl_decoder d2;
    echo e;

    c->frames++;
    FUZZ_CHECK(f->len <= FL_MAX_PAYLOAD);
    FUZZ_CHECK(f->payload != NULL);

    /* A delivered frame must survive a full round trip unchanged. */
    FUZZ_CHECK(fl_encode(f->type, f->seq, f->payload, f->len, buf, sizeof buf, &n) == FL_OK);
    FUZZ_CHECK(n == FL_FRAME_SIZE(f->len));
    memset(&e, 0, sizeof e);
    fl_decoder_init(&d2);
    fl_decoder_feed(&d2, buf, n, on_echo, &e);
    FUZZ_CHECK(e.seen == 1);
    FUZZ_CHECK(e.frame.type == f->type && e.frame.seq == f->seq && e.frame.len == f->len);
    FUZZ_CHECK(f->len == 0 || memcmp(e.payload, f->payload, f->len) == 0);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    static fl_decoder dec;
    ctx c = {0};
    size_t pos = 1, chunk_no = 0;
    size_t chunk, timeout_every;

    if (size == 0) {
        return 0;
    }
    chunk = (size_t)(data[0] & 0x0Fu) + 1u;          /* 1..16 bytes per feed */
    timeout_every = (size_t)((data[0] >> 4) & 0x0Fu); /* 0 means never */

    fl_decoder_init(&dec);
    while (pos < size) {
        size_t n = size - pos < chunk ? size - pos : chunk;
        fl_decoder_feed(&dec, data + pos, n, on_frame, &c);
        pos += n;
        chunk_no++;
        if (timeout_every != 0 && chunk_no % timeout_every == 0) {
            fl_decoder_timeout(&dec, on_frame, &c);
        }
    }
    fl_decoder_timeout(&dec, on_frame, &c);

    FUZZ_CHECK(!fl_decoder_busy(&dec));
    FUZZ_CHECK(dec.stats.rx_frames == c.frames);
    return 0;
}
