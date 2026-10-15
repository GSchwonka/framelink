/*
 * Unit tests for the framelink codec: checksums, encoder and decoder.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#include <string.h>

#include "framelink.h"
#include "frames_vectors.h"
#include "unity.h"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

#define CAP_MAX 4096u

typedef struct captured {
    uint8_t type;
    uint8_t seq;
    uint8_t len;
    uint8_t payload[FL_MAX_PAYLOAD + 1];
} captured;

static captured cap[CAP_MAX];
static size_t ncap;
static fl_decoder dec;

static void on_frame(void *user, const fl_frame *f)
{
    (void)user;
    TEST_ASSERT_TRUE_MESSAGE(f->len <= FL_MAX_PAYLOAD, "frame longer than FL_MAX_PAYLOAD");
    if (ncap < CAP_MAX) {
        cap[ncap].type = f->type;
        cap[ncap].seq = f->seq;
        cap[ncap].len = f->len;
        if (f->len > 0) {
            memcpy(cap[ncap].payload, f->payload, f->len);
        }
    }
    ncap++;
}

static void feed(const uint8_t *data, size_t len)
{
    fl_decoder_feed(&dec, data, len, on_frame, NULL);
}

static void flush(void)
{
    fl_decoder_timeout(&dec, on_frame, NULL);
}

static size_t encode(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len,
                     uint8_t *out, size_t cap_bytes)
{
    size_t out_len = 0;
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_encode(type, seq, payload, len, out, cap_bytes, &out_len));
    return out_len;
}

static void assert_captured(size_t i, uint8_t type, uint8_t seq, const uint8_t *payload, size_t len)
{
    TEST_ASSERT_TRUE(i < ncap);
    TEST_ASSERT_EQUAL_HEX8(type, cap[i].type);
    TEST_ASSERT_EQUAL_HEX8(seq, cap[i].seq);
    TEST_ASSERT_EQUAL_UINT(len, cap[i].len);
    if (len > 0) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, cap[i].payload, len);
    }
}

/* Deterministic PRNG so every failure is reproducible. */
static uint32_t rng_state;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Two frames used by many tests. Neither payload contains SYNC. */
static const uint8_t PAYLOAD_A[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
static const uint8_t PAYLOAD_B[] = {'f', 'r', 'a', 'm', 'e'};
static uint8_t frame_a[FL_FRAME_SIZE(sizeof PAYLOAD_A)];
static uint8_t frame_b[FL_FRAME_SIZE(sizeof PAYLOAD_B)];

void setUp(void)
{
    fl_decoder_init(&dec);
    ncap = 0;
    rng_state = 0x2545F491u;
    encode(0x01, 0x11, PAYLOAD_A, sizeof PAYLOAD_A, frame_a, sizeof frame_a);
    encode(0x02, 0x22, PAYLOAD_B, sizeof PAYLOAD_B, frame_b, sizeof frame_b);
}

void tearDown(void) {}

/* ------------------------------------------------------------------------ */
/* Checksums                                                                */
/* ------------------------------------------------------------------------ */

static void test_crc8_check_value(void)
{
    const char *s = FL_VEC_CRC_INPUT;
    TEST_ASSERT_EQUAL_HEX8(FL_VEC_CRC8_CHECK, fl_crc8((const uint8_t *)s, strlen(s)));
}

static void test_crc16_check_value(void)
{
    const char *s = FL_VEC_CRC_INPUT;
    TEST_ASSERT_EQUAL_HEX16(FL_VEC_CRC16_CHECK, fl_crc16((const uint8_t *)s, strlen(s)));
}

static void test_crc16_update_matches_block(void)
{
    uint8_t data[300];
    uint16_t crc = 0xFFFFu;
    size_t i;
    for (i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)rng();
        crc = fl_crc16_update(crc, data[i]);
    }
    TEST_ASSERT_EQUAL_HEX16(fl_crc16(data, sizeof data), crc);
}

static void test_crc_of_empty_input_is_init_value(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x00, fl_crc8(NULL, 0));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, fl_crc16(NULL, 0));
}

/* The header codeword is LEN TYPE SEQ HCRC, 32 bits. Every error of 1, 2 or
 * 3 bits in it must be detected. CRCs are linear, so checking one header
 * covers all headers. 32 + 496 + 4960 = 5488 patterns. */
static void test_hcrc_detects_every_1_2_3_bit_header_error(void)
{
    uint8_t hdr[4] = {0x2A, 0x01, 0x7F, 0};
    unsigned a, b, c, patterns = 0;
    hdr[3] = fl_crc8(hdr, 3);

    for (a = 0; a < 32; a++) {
        for (b = a; b < 32; b++) {
            for (c = b; c < 32; c++) {
                uint8_t e[4];
                memcpy(e, hdr, 4);
                /* a == b == c is a 1-bit error, a < b == c a 2-bit error. */
                if (!((a == b && b == c) || (a < b && b == c) || (a < b && b < c))) {
                    continue;
                }
                e[a / 8] ^= (uint8_t)(1u << (a % 8));
                if (b != a) {
                    e[b / 8] ^= (uint8_t)(1u << (b % 8));
                }
                if (c != b) {
                    e[c / 8] ^= (uint8_t)(1u << (c % 8));
                }
                patterns++;
                TEST_ASSERT_NOT_EQUAL(e[3], fl_crc8(e, 3));
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT(5488u, patterns);
}

/* ------------------------------------------------------------------------ */
/* Encoder                                                                  */
/* ------------------------------------------------------------------------ */

static void test_vectors_encode(void)
{
    size_t i;
    for (i = 0; i < FL_VECTOR_COUNT; i++) {
        const fl_vector *v = &fl_vectors[i];
        uint8_t out[FL_FRAME_SIZE(255)];
        size_t out_len = 0;
        fl_err err = fl_encode(v->type, v->seq, v->payload, v->payload_len, out, sizeof out, &out_len);
        if (v->payload_len > FL_MAX_PAYLOAD) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(FL_ETOOBIG, err, v->name);
            continue;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(FL_OK, err, v->name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(v->encoded_len, out_len, v->name);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(v->encoded, out, v->encoded_len, v->name);
    }
}

static void test_encode_rejects_invalid_arguments(void)
{
    uint8_t out[FL_MAX_FRAME + 1];
    uint8_t big[256] = {0};
    size_t out_len = 1234;

    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_encode(1, 0, PAYLOAD_A, 1, NULL, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_encode(1, 0, NULL, 1, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_INT(FL_ETOOBIG, fl_encode(1, 0, big, FL_MAX_PAYLOAD + 1u, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_INT(FL_ENOSPC, fl_encode(1, 0, PAYLOAD_A, 4, out, FL_FRAME_SIZE(4) - 1u, &out_len));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1234u, out_len, "out_len must be untouched on error");

    /* Exact capacity and a NULL out_len are both fine. */
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_encode(1, 0, PAYLOAD_A, 4, out, FL_FRAME_SIZE(4), NULL));
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_encode(1, 0, NULL, 0, out, FL_OVERHEAD, NULL));
}

/* ------------------------------------------------------------------------ */
/* Decoder: clean input                                                     */
/* ------------------------------------------------------------------------ */

static void test_vectors_decode(void)
{
    size_t i;
    for (i = 0; i < FL_VECTOR_COUNT; i++) {
        const fl_vector *v = &fl_vectors[i];
        setUp();
        feed(v->encoded, v->encoded_len);
        if (v->payload_len > FL_MAX_PAYLOAD) {
            /* The oversized header is rejected. Payload bytes that happen to
             * be SYNC are then tried as frame starts too, so there may be
             * more than one header error, but never a frame. */
            flush();
            TEST_ASSERT_EQUAL_UINT_MESSAGE(0, ncap, v->name);
            TEST_ASSERT_TRUE_MESSAGE(dec.stats.hdr_errors >= 1, v->name);
            continue;
        }
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, ncap, v->name);
        assert_captured(0, v->type, v->seq, v->payload, v->payload_len);
        TEST_ASSERT_FALSE(fl_decoder_busy(&dec));
    }
}

static void test_roundtrip_boundary_sizes(void)
{
    static const size_t sizes[] = {0, 1, 2, 254, 255};
    size_t i;
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        uint8_t payload[255];
        uint8_t out[FL_FRAME_SIZE(255)];
        size_t len = sizes[i] > FL_MAX_PAYLOAD ? FL_MAX_PAYLOAD : sizes[i];
        size_t j, n;
        for (j = 0; j < len; j++) {
            payload[j] = (uint8_t)rng();
        }
        setUp();
        n = encode(0x33, (uint8_t)i, payload, len, out, sizeof out);
        feed(out, n);
        TEST_ASSERT_EQUAL_UINT(1, ncap);
        assert_captured(0, 0x33, (uint8_t)i, payload, len);
    }
}

static void test_back_to_back_frames_split_at_every_boundary(void)
{
    uint8_t stream[sizeof frame_a + sizeof frame_b];
    size_t split;
    memcpy(stream, frame_a, sizeof frame_a);
    memcpy(stream + sizeof frame_a, frame_b, sizeof frame_b);

    for (split = 0; split <= sizeof stream; split++) {
        setUp();
        feed(stream, split);
        feed(stream + split, sizeof stream - split);
        TEST_ASSERT_EQUAL_UINT(2, ncap);
        assert_captured(0, 0x01, 0x11, PAYLOAD_A, sizeof PAYLOAD_A);
        assert_captured(1, 0x02, 0x22, PAYLOAD_B, sizeof PAYLOAD_B);
    }
}

static void test_byte_by_byte_feed(void)
{
    size_t i;
    for (i = 0; i < sizeof frame_a; i++) {
        feed(&frame_a[i], 1);
        TEST_ASSERT_EQUAL_UINT(i + 1 == sizeof frame_a ? 1u : 0u, ncap);
    }
    assert_captured(0, 0x01, 0x11, PAYLOAD_A, sizeof PAYLOAD_A);
}

static void test_sync_bytes_inside_payload_and_crc(void)
{
    uint8_t payload[16];
    uint8_t out[FL_FRAME_SIZE(16)];
    unsigned seq;
    int found_sync_in_crc = 0;
    memset(payload, FL_SYNC, sizeof payload);

    /* All-SYNC payload. */
    feed(out, encode(0x05, 0x00, payload, sizeof payload, out, sizeof out));
    TEST_ASSERT_EQUAL_UINT(1, ncap);
    assert_captured(0, 0x05, 0x00, payload, sizeof payload);

    /* Find a sequence number whose CRC contains SYNC and check it decodes. */
    for (seq = 0; seq < 256 && !found_sync_in_crc; seq++) {
        size_t n = encode(0x05, (uint8_t)seq, PAYLOAD_A, sizeof PAYLOAD_A, out, sizeof out);
        if (out[n - 1] == FL_SYNC || out[n - 2] == FL_SYNC) {
            setUp();
            feed(out, n);
            feed(frame_b, sizeof frame_b);
            TEST_ASSERT_EQUAL_UINT(2, ncap);
            assert_captured(0, 0x05, (uint8_t)seq, PAYLOAD_A, sizeof PAYLOAD_A);
            assert_captured(1, 0x02, 0x22, PAYLOAD_B, sizeof PAYLOAD_B);
            found_sync_in_crc = 1;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_sync_in_crc, "no frame with SYNC in its CRC found");
}

/* ------------------------------------------------------------------------ */
/* Decoder: corrupted input                                                 */
/* ------------------------------------------------------------------------ */

static void test_garbage_without_sync_is_counted_and_ignored(void)
{
    uint8_t garbage[100];
    size_t i;
    for (i = 0; i < sizeof garbage; i++) {
        uint8_t b = (uint8_t)rng();
        garbage[i] = b == FL_SYNC ? 0x00 : b;
    }
    feed(garbage, sizeof garbage);
    feed(frame_a, sizeof frame_a);
    TEST_ASSERT_EQUAL_UINT(1, ncap);
    TEST_ASSERT_EQUAL_UINT32(sizeof garbage, dec.stats.hunt_bytes);
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.rx_frames);
}

static void test_random_garbage_prefix_never_loses_the_frame(void)
{
    unsigned round;
    for (round = 0; round < 500; round++) {
        uint8_t garbage[64];
        size_t len = rng() % sizeof garbage, i;
        for (i = 0; i < len; i++) {
            /* Bias towards SYNC so false candidates are common. */
            garbage[i] = (rng() % 4 == 0) ? FL_SYNC : (uint8_t)rng();
        }
        fl_decoder_init(&dec);
        ncap = 0;
        feed(garbage, len);
        feed(frame_a, sizeof frame_a);
        flush();
        /* A false candidate made of garbage could only pass both CRCs with
         * probability around 2^-24, and the seed is fixed, so exactly one
         * frame must arrive and it must be frame A. */
        TEST_ASSERT_EQUAL_UINT(1, ncap);
        assert_captured(0, 0x01, 0x11, PAYLOAD_A, sizeof PAYLOAD_A);
    }
}

/* Every single-bit error in the header of frame A is caught by the header
 * CRC, and frame B behind it is delivered immediately, without waiting for
 * a timeout or for LEN bytes to arrive. This is the desync bug class. */
static void test_corrupt_header_does_not_delay_next_frame(void)
{
    unsigned byte, bit;
    for (byte = 1; byte <= 4; byte++) {
        for (bit = 0; bit < 8; bit++) {
            uint8_t bad[sizeof frame_a];
            memcpy(bad, frame_a, sizeof bad);
            bad[byte] ^= (uint8_t)(1u << bit);
            setUp();
            feed(bad, sizeof bad);
            feed(frame_b, sizeof frame_b);
            TEST_ASSERT_EQUAL_UINT(1, ncap);
            assert_captured(0, 0x02, 0x22, PAYLOAD_B, sizeof PAYLOAD_B);
            TEST_ASSERT_EQUAL_UINT32(1, dec.stats.hdr_errors);
            TEST_ASSERT_FALSE(fl_decoder_busy(&dec));
        }
    }
}

static void test_corrupt_payload_is_rejected_and_next_frame_kept(void)
{
    uint8_t bad[sizeof frame_a];
    memcpy(bad, frame_a, sizeof bad);
    bad[FL_HEADER_LEN + 3] ^= 0x04;
    feed(bad, sizeof bad);
    feed(frame_b, sizeof frame_b);
    TEST_ASSERT_EQUAL_UINT(1, ncap);
    assert_captured(0, 0x02, 0x22, PAYLOAD_B, sizeof PAYLOAD_B);
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.crc_errors);
}

/* A frame cut off at any point, followed by two valid frames. Both valid
 * frames must arrive, in order, even when the truncated header already
 * passed and the decoder is waiting for payload bytes. */
static void test_truncated_frame_followed_by_valid_frames(void)
{
    size_t cut;
    for (cut = 1; cut < sizeof frame_a; cut++) {
        setUp();
        feed(frame_a, cut);
        feed(frame_b, sizeof frame_b);
        feed(frame_a, sizeof frame_a);
        flush();
        TEST_ASSERT_EQUAL_UINT(2, ncap);
        assert_captured(0, 0x02, 0x22, PAYLOAD_B, sizeof PAYLOAD_B);
        assert_captured(1, 0x01, 0x11, PAYLOAD_A, sizeof PAYLOAD_A);
    }
}

/* Two frames squashed inside a header that claims a long payload: both
 * must be recovered by rescanning once the long candidate fails. */
static void test_frames_inside_failed_candidate_are_recovered(void)
{
    uint8_t stream[FL_HEADER_LEN + sizeof frame_a + sizeof frame_b + 64];
    /* Long enough to swallow both frames, valid in any build. */
    uint8_t hdr[4] = {FL_MAX_PAYLOAD < 200 ? FL_MAX_PAYLOAD : 200, 0x01, 0x00, 0};
    size_t n = 0;
    TEST_ASSERT_TRUE_MESSAGE(hdr[0] > sizeof frame_a + sizeof frame_b, "FL_MAX_PAYLOAD too small for this test");
    hdr[3] = fl_crc8(hdr, 3);

    stream[n++] = FL_SYNC;
    memcpy(&stream[n], hdr, 4);
    n += 4;
    memcpy(&stream[n], frame_a, sizeof frame_a);
    n += sizeof frame_a;
    memcpy(&stream[n], frame_b, sizeof frame_b);
    n += sizeof frame_b;

    feed(stream, n);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, ncap, "frames should wait inside the long candidate");
    TEST_ASSERT_TRUE(fl_decoder_busy(&dec));
    flush();
    TEST_ASSERT_EQUAL_UINT(2, ncap);
    assert_captured(0, 0x01, 0x11, PAYLOAD_A, sizeof PAYLOAD_A);
    assert_captured(1, 0x02, 0x22, PAYLOAD_B, sizeof PAYLOAD_B);
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.resyncs);
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.timeouts);
}

/* Any error of 1, 2 or 3 flipped bits anywhere in a frame is detected: the
 * frame is never delivered. Exhaustive over a 23-byte frame, 184 bits. */
static void test_every_1_2_3_bit_frame_error_is_detected(void)
{
    uint8_t payload[16];
    uint8_t good[FL_FRAME_SIZE(16)];
    const unsigned nbits = (unsigned)sizeof good * 8u;
    unsigned a, b, c;
    unsigned long patterns = 0;
    size_t i;
    for (i = 0; i < sizeof payload; i++) {
        payload[i] = (uint8_t)rng();
    }
    encode(0x09, 0x42, payload, sizeof payload, good, sizeof good);

    for (a = 0; a < nbits; a++) {
        for (b = a; b < nbits; b++) {
            for (c = b; c < nbits; c++) {
                uint8_t e[sizeof good];
                if (!((a == b && b == c) || (a < b && b == c) || (a < b && b < c))) {
                    continue;
                }
                memcpy(e, good, sizeof e);
                e[a / 8] ^= (uint8_t)(1u << (a % 8));
                if (b != a) {
                    e[b / 8] ^= (uint8_t)(1u << (b % 8));
                }
                if (c != b) {
                    e[c / 8] ^= (uint8_t)(1u << (c % 8));
                }
                fl_decoder_init(&dec);
                ncap = 0;
                feed(e, sizeof e);
                flush();
                patterns++;
                if (ncap != 0) {
                    TEST_FAIL_MESSAGE("corrupted frame was accepted");
                }
            }
        }
    }
    /* C(184,1) + C(184,2) + C(184,3) */
    TEST_ASSERT_EQUAL_UINT32(184ul + 16836ul + 1021384ul, patterns);
}

/* ------------------------------------------------------------------------ */
/* Decoder: timeout and statistics                                          */
/* ------------------------------------------------------------------------ */

static void test_timeout_discards_partial_frame(void)
{
    feed(frame_a, sizeof frame_a - 1);
    TEST_ASSERT_TRUE(fl_decoder_busy(&dec));
    flush();
    TEST_ASSERT_FALSE(fl_decoder_busy(&dec));
    TEST_ASSERT_EQUAL_UINT(0, ncap);
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.timeouts);

    /* A timeout with nothing buffered is not counted. */
    flush();
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.timeouts);

    feed(frame_b, sizeof frame_b);
    TEST_ASSERT_EQUAL_UINT(1, ncap);
}

static void test_null_callback_still_counts_frames(void)
{
    fl_decoder_feed(&dec, frame_a, sizeof frame_a, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.rx_frames);
}

#if FL_MAX_PAYLOAD < 255
static void test_len_above_build_limit_is_a_header_error(void)
{
    uint8_t stream[8] = {FL_SYNC, FL_MAX_PAYLOAD + 1, 0x01, 0x00, 0};
    stream[4] = fl_crc8(&stream[1], 3);
    feed(stream, 5);
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.hdr_errors);
    TEST_ASSERT_FALSE(fl_decoder_busy(&dec));
}
#endif

/* ------------------------------------------------------------------------ */
/* Property test: random frames through a corrupting channel                */
/* ------------------------------------------------------------------------ */

#define PROP_FRAMES 2000u
#define PROP_MAXLEN 40u

/* Every delivered frame must be one of the sent frames, in order, and every
 * frame that crossed the channel untouched must be delivered. */
static void test_property_corrupting_channel(void)
{
    static uint8_t stream[PROP_FRAMES * FL_FRAME_SIZE(PROP_MAXLEN)];
    static uint8_t sent_payload[PROP_FRAMES][PROP_MAXLEN];
    static uint8_t sent_len[PROP_FRAMES];
    static uint8_t intact[PROP_FRAMES];
    size_t n = 0, f, k, delivered_intact = 0, total_intact = 0;

    for (f = 0; f < PROP_FRAMES; f++) {
        size_t len = rng() % (PROP_MAXLEN + 1), i, start = n;
        for (i = 0; i < len; i++) {
            sent_payload[f][i] = (uint8_t)rng();
        }
        sent_len[f] = (uint8_t)len;
        n += encode(0x01, (uint8_t)f, sent_payload[f], len, &stream[n], sizeof stream - n);
        intact[f] = 1;
        /* Corrupt about 1 byte in 50. */
        for (i = start; i < n; i++) {
            if (rng() % 50 == 0) {
                stream[i] ^= (uint8_t)(1u << (rng() % 8));
                intact[f] = 0;
            }
        }
        total_intact += intact[f];
    }

    feed(stream, n);
    flush();

    /* Walk delivered frames against sent frames. */
    f = 0;
    for (k = 0; k < ncap && k < CAP_MAX; k++) {
        while (f < PROP_FRAMES && !(cap[k].seq == (uint8_t)f && cap[k].len == sent_len[f] &&
                                    memcmp(cap[k].payload, sent_payload[f], sent_len[f]) == 0)) {
            TEST_ASSERT_FALSE_MESSAGE(intact[f], "an intact frame was skipped");
            f++;
        }
        TEST_ASSERT_TRUE_MESSAGE(f < PROP_FRAMES, "delivered frame does not match any sent frame");
        delivered_intact += intact[f];
        f++;
    }
    for (; f < PROP_FRAMES; f++) {
        TEST_ASSERT_FALSE_MESSAGE(intact[f], "an intact frame at the end was lost");
    }
    TEST_ASSERT_EQUAL_UINT(total_intact, delivered_intact);
    TEST_ASSERT_TRUE_MESSAGE(total_intact > PROP_FRAMES / 4, "corruption rate too high to be useful");
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc8_check_value);
    RUN_TEST(test_crc16_check_value);
    RUN_TEST(test_crc16_update_matches_block);
    RUN_TEST(test_crc_of_empty_input_is_init_value);
    RUN_TEST(test_hcrc_detects_every_1_2_3_bit_header_error);
    RUN_TEST(test_vectors_encode);
    RUN_TEST(test_encode_rejects_invalid_arguments);
    RUN_TEST(test_vectors_decode);
    RUN_TEST(test_roundtrip_boundary_sizes);
    RUN_TEST(test_back_to_back_frames_split_at_every_boundary);
    RUN_TEST(test_byte_by_byte_feed);
    RUN_TEST(test_sync_bytes_inside_payload_and_crc);
    RUN_TEST(test_garbage_without_sync_is_counted_and_ignored);
    RUN_TEST(test_random_garbage_prefix_never_loses_the_frame);
    RUN_TEST(test_corrupt_header_does_not_delay_next_frame);
    RUN_TEST(test_corrupt_payload_is_rejected_and_next_frame_kept);
    RUN_TEST(test_truncated_frame_followed_by_valid_frames);
    RUN_TEST(test_frames_inside_failed_candidate_are_recovered);
    RUN_TEST(test_every_1_2_3_bit_frame_error_is_detected);
    RUN_TEST(test_timeout_discards_partial_frame);
    RUN_TEST(test_null_callback_still_counts_frames);
#if FL_MAX_PAYLOAD < 255
    RUN_TEST(test_len_above_build_limit_is_a_header_error);
#endif
    RUN_TEST(test_property_corrupting_channel);
    return UNITY_END();
}
