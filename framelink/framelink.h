/*
 * framelink - reliable framed messaging over unreliable byte streams.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 *
 * Wire format and behaviour are defined in docs/SPEC.md. This header exposes
 * the codec layer: checksums, the frame encoder and the resynchronising
 * decoder. The codec is usable on its own, without the link layer.
 *
 * Design rules for this library:
 *   - C99, no dependencies beyond <stddef.h> and <stdint.h>.
 *   - No dynamic allocation. All memory belongs to the caller.
 *   - No global state. Every function works on caller-owned structures.
 */
#ifndef FRAMELINK_H
#define FRAMELINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Version                                                                  */
/* ------------------------------------------------------------------------ */

#define FL_VERSION_MAJOR 0
#define FL_VERSION_MINOR 1
#define FL_VERSION_PATCH 0

/** Wire protocol version, sent in HELLO frames. */
#define FL_PROTOCOL_VERSION 1u

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */

/**
 * Largest payload this build accepts, 0..255. Lowering it shrinks every
 * buffer. Frames that announce a longer payload are rejected as header errors.
 */
#ifndef FL_MAX_PAYLOAD
#define FL_MAX_PAYLOAD 255
#endif

#if FL_MAX_PAYLOAD < 0 || FL_MAX_PAYLOAD > 255
#error "FL_MAX_PAYLOAD must be in the range 0..255"
#endif

/* Define FL_CRC_TABLE to use a 512-byte lookup table for CRC16. It is about
 * eight times faster per byte than the default bitwise version and costs
 * flash. Both produce identical results. */

/* ------------------------------------------------------------------------ */
/* Wire constants                                                           */
/* ------------------------------------------------------------------------ */

#define FL_SYNC        0x7Eu
#define FL_HEADER_LEN  5u  /* SYNC LEN TYPE SEQ HCRC */
#define FL_TRAILER_LEN 2u  /* CRC_LO CRC_HI */
#define FL_OVERHEAD    (FL_HEADER_LEN + FL_TRAILER_LEN)
#define FL_MAX_FRAME   (FL_OVERHEAD + FL_MAX_PAYLOAD)

/** Encoded size of a frame carrying `payload_len` bytes. */
#define FL_FRAME_SIZE(payload_len) ((size_t)(payload_len) + FL_OVERHEAD)

/* Frame types. 0x00..0xEF belong to the application. */
#define FL_TYPE_USER_MAX  0xEFu
#define FL_TYPE_ACK       0xF0u
#define FL_TYPE_NAK       0xF1u
#define FL_TYPE_CREDIT    0xF2u
#define FL_TYPE_HELLO     0xF3u
#define FL_TYPE_HELLO_ACK 0xF4u
#define FL_TYPE_PING      0xF5u
#define FL_TYPE_PONG      0xF6u

/* ------------------------------------------------------------------------ */
/* Errors                                                                   */
/* ------------------------------------------------------------------------ */

typedef enum fl_err {
    FL_OK = 0,
    FL_EINVAL = -1,  /* invalid argument */
    FL_ENOSPC = -2,  /* output buffer too small */
    FL_ETOOBIG = -3, /* payload longer than FL_MAX_PAYLOAD */
    FL_EBUSY = -4    /* link layer: window or peer credits exhausted */
} fl_err;

/* ------------------------------------------------------------------------ */
/* Checksums                                                                */
/* ------------------------------------------------------------------------ */

/** CRC-8/SMBUS: poly 0x07, init 0x00, no reflection, no final XOR. */
uint8_t fl_crc8(const uint8_t *data, size_t len);

/** CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR. */
uint16_t fl_crc16(const uint8_t *data, size_t len);

/** Feed one more byte into a running CRC-16/CCITT-FALSE. Start from 0xFFFF. */
uint16_t fl_crc16_update(uint16_t crc, uint8_t byte);

/* ------------------------------------------------------------------------ */
/* Encoder                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * Encode one frame into `out`.
 *
 * @param type        frame type
 * @param seq         sequence number, 0 for control frames
 * @param payload     payload bytes, may be NULL when `payload_len` is 0
 * @param payload_len number of payload bytes, at most FL_MAX_PAYLOAD
 * @param out         destination buffer
 * @param out_cap     capacity of `out`, at least FL_FRAME_SIZE(payload_len)
 * @param out_len     receives the encoded length on success, may be NULL
 */
fl_err fl_encode(uint8_t type, uint8_t seq,
                 const uint8_t *payload, size_t payload_len,
                 uint8_t *out, size_t out_cap, size_t *out_len);

/* ------------------------------------------------------------------------ */
/* Decoder                                                                  */
/* ------------------------------------------------------------------------ */

/** A decoded frame. `payload` is only valid during the callback. */
typedef struct fl_frame {
    uint8_t type;
    uint8_t seq;
    uint8_t len;
    const uint8_t *payload;
} fl_frame;

/**
 * Called once per valid frame. The callback must not feed or reset the
 * decoder that invoked it.
 */
typedef void (*fl_frame_cb)(void *user, const fl_frame *frame);

/** Decoder counters, named as in docs/SPEC.md section 8. */
typedef struct fl_decoder_stats {
    uint32_t rx_frames;  /* frames that passed both CRCs */
    uint32_t hunt_bytes; /* bytes discarded while looking for SYNC */
    uint32_t hdr_errors; /* header CRC failures, or LEN above FL_MAX_PAYLOAD */
    uint32_t crc_errors; /* frame CRC failures */
    uint32_t resyncs;    /* frames recovered from bytes of a failed candidate */
    uint32_t timeouts;   /* partial frames abandoned by fl_decoder_timeout */
} fl_decoder_stats;

/**
 * Decoder state. Treat the fields as private. The struct is public only so
 * that callers can allocate it statically.
 */
typedef struct fl_decoder {
    uint8_t buf[FL_MAX_FRAME]; /* bytes of the current candidate frame */
    uint16_t n;                /* bytes buffered */
    uint16_t need;             /* full frame length, 0 until header passes */
    uint8_t resync_pending;    /* buffered bytes came from a failed candidate */
    fl_decoder_stats stats;
} fl_decoder;

/** Reset the decoder to its initial state, including the counters. */
void fl_decoder_init(fl_decoder *dec);

/**
 * Feed received bytes. `on_frame` is called for every complete, valid frame,
 * in stream order. Corrupted or truncated candidates are discarded, and the
 * bytes they buffered are rescanned so that no valid frame starting inside
 * them is lost.
 */
void fl_decoder_feed(fl_decoder *dec, const uint8_t *data, size_t len,
                     fl_frame_cb on_frame, void *user);

/**
 * Tell the decoder that no byte has arrived for longer than the frame
 * timeout. Any partial frame is abandoned and its bytes are rescanned.
 * Complete frames found in those bytes are still delivered.
 */
void fl_decoder_timeout(fl_decoder *dec, fl_frame_cb on_frame, void *user);

/** True while the decoder holds a partial frame. Use it to arm a timeout. */
int fl_decoder_busy(const fl_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* FRAMELINK_H */
