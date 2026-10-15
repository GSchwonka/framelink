/*
 * Fuzz harness for encode and decode round trips.
 *
 * Input layout: TYPE SEQ SPLIT PAYLOAD...
 * The payload is truncated to FL_MAX_PAYLOAD. The encoded frame is fed to the
 * decoder in two pieces, split at SPLIT, and must come back exactly once and
 * unchanged.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#include <stdint.h>
#include <string.h>

#include "framelink.h"
#include "fuzz_check.h"

typedef struct result {
    int count;
    fl_frame frame;
    uint8_t payload[FL_MAX_PAYLOAD + 1];
} result;

static void on_frame(void *user, const fl_frame *f)
{
    result *r = (result *)user;
    r->count++;
    r->frame = *f;
    if (f->len > 0) {
        memcpy(r->payload, f->payload, f->len);
    }
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    static fl_decoder dec;
    uint8_t frame[FL_MAX_FRAME];
    size_t frame_len = 0, len, split;
    const uint8_t *payload;
    result r;

    if (size < 3) {
        return 0;
    }
    payload = data + 3;
    len = size - 3;
    if (len > FL_MAX_PAYLOAD) {
        FUZZ_CHECK(fl_encode(data[0], data[1], payload, len, frame, sizeof frame, NULL) == FL_ETOOBIG);
        len = FL_MAX_PAYLOAD;
    }

    FUZZ_CHECK(fl_encode(data[0], data[1], payload, len, frame, sizeof frame, &frame_len) == FL_OK);
    FUZZ_CHECK(frame_len == FL_FRAME_SIZE(len));
    FUZZ_CHECK(frame[0] == FL_SYNC && frame[1] == len);

    split = data[2] % (frame_len + 1);
    memset(&r, 0, sizeof r);
    fl_decoder_init(&dec);
    fl_decoder_feed(&dec, frame, split, on_frame, &r);
    fl_decoder_feed(&dec, frame + split, frame_len - split, on_frame, &r);

    FUZZ_CHECK(r.count == 1);
    FUZZ_CHECK(r.frame.type == data[0] && r.frame.seq == data[1] && r.frame.len == len);
    FUZZ_CHECK(len == 0 || memcmp(r.payload, payload, len) == 0);
    FUZZ_CHECK(!fl_decoder_busy(&dec));
    FUZZ_CHECK(dec.stats.hdr_errors == 0 && dec.stats.crc_errors == 0);
    return 0;
}
