/*
 * framelink - reliable framed messaging over unreliable byte streams.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 *
 * Wire format and behaviour are defined in docs/SPEC.md. The header has two
 * layers:
 *   - the codec: checksums, the frame encoder and the resynchronising
 *     decoder, usable on their own;
 *   - the link: handshake, acknowledgements, retransmission and credit-based
 *     flow control, built on the codec.
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
#define FL_VERSION_MINOR 2
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
    FL_EBUSY = -4,   /* link layer: window or peer credits exhausted */
    FL_EAGAIN = -5,  /* link layer: nothing to receive yet */
    FL_ENOTCONN = -6 /* link layer: handshake not complete */
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
    fl_frame_cb on_corrupt;    /* optional, see fl_decoder_set_corrupt_cb */
    void *corrupt_user;
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

/**
 * Optional hook for candidates whose header passed its CRC but whose body
 * failed CRC16. The header fields can be trusted, the payload cannot. The
 * link layer uses this to send a NAK at once instead of waiting for a
 * timeout. Call after fl_decoder_init, which clears it.
 */
void fl_decoder_set_corrupt_cb(fl_decoder *dec, fl_frame_cb on_corrupt, void *user);

/* ======================================================================== */
/* Link layer (SPEC.md section 5)                                           */
/* ======================================================================== */

/** Bytes per receive queue slot: LEN, TYPE and the payload. */
#define FL_RX_SLOT_SIZE (FL_MAX_PAYLOAD + 2u)

/** Transmit buffer size for a send window of `window` frames. */
#define FL_TX_BUF_SIZE(window) ((size_t)(window) * FL_MAX_FRAME)

/** Receive buffer size for a queue of `slots` frames, at most 255. */
#define FL_RX_BUF_SIZE(slots) ((size_t)(slots) * FL_RX_SLOT_SIZE)

/** Things the link reports to the application through `on_event`. */
typedef enum fl_event {
    FL_EV_CONNECTED = 1,    /* handshake complete, arg 0 */
    FL_EV_DELIVERED,        /* the peer acknowledged a data frame, arg = its SEQ */
    FL_EV_LINK_DOWN,        /* retries exhausted, arg = frames never acknowledged */
    FL_EV_PEER_RESET,       /* the peer started a new session, arg = frames never acknowledged */
    FL_EV_PONG,             /* a PONG arrived, arg = its payload length */
    FL_EV_VERSION_MISMATCH  /* the peer uses another protocol version, arg = that version */
} fl_event;

/**
 * Link configuration. Buffers and callbacks belong to the caller and must
 * outlive the link.
 */
typedef struct fl_config {
    uint8_t *tx_buf;           /* at least FL_TX_BUF_SIZE(window) bytes */
    size_t tx_buf_len;
    uint8_t *rx_buf;           /* FL_RX_BUF_SIZE(n) bytes for an n-frame queue */
    size_t rx_buf_len;
    uint8_t window;            /* frames in flight, 1..127 */
    uint8_t max_retries;       /* retransmissions of one frame before link down, >= 1 */
    uint16_t rto_ms;           /* retransmit and handshake interval, > 0 */
    uint16_t frame_timeout_ms; /* inter-byte timeout for partial frames, 0 = rto_ms */
    uint8_t session_seed;      /* first session ID. Use a random value per boot */

    /* Required. Write all `len` bytes, or as many as possible. Bytes that
     * are not written are recovered by retransmission. */
    int (*write_bytes)(void *user, const uint8_t *data, size_t len);
    /* Optional. Return up to `cap` received bytes, 0 when none are waiting.
     * Without it, pass received bytes to fl_input yourself. */
    size_t (*read_bytes)(void *user, uint8_t *data, size_t cap);
    /* Optional. May call fl_send and fl_recv, but not fl_poll or fl_input. */
    void (*on_event)(void *user, fl_event event, uint8_t arg);
    void *user;
} fl_config;

/** Link and decoder counters, named as in docs/SPEC.md section 8. */
typedef struct fl_stats {
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t hunt_bytes;
    uint32_t hdr_errors;
    uint32_t crc_errors;
    uint32_t resyncs;
    uint32_t timeouts;
    uint32_t retransmits;
    uint32_t dup_drops;
    uint32_t overflows;
    uint32_t unknown_type;
    uint32_t link_down;
} fl_stats;

/**
 * Link state. Treat the fields as private. The struct is public only so
 * that callers can allocate it statically.
 */
typedef struct fl_link {
    fl_config cfg;
    fl_decoder dec;
    uint32_t now;             /* time of the last fl_poll */
    uint32_t last_rx;         /* time bytes last arrived */

    /* Handshake */
    uint8_t connected;
    uint8_t session;          /* our current session ID */
    uint8_t peer_session;
    uint8_t peer_session_valid;
    uint8_t hello_now;        /* send HELLO on the next poll */
    uint32_t hello_due;

    /* Sender */
    uint8_t snd_una;
    uint8_t snd_nxt;
    uint8_t tx_head;          /* slot holding snd_una */
    uint8_t peer_credits;
    uint8_t retries;
    uint8_t timer_on;
    uint8_t want_credit;      /* a send was refused for lack of credits */
    uint32_t rto_due;
    uint32_t probe_due;

    /* Receiver */
    uint8_t rcv_nxt;
    uint8_t rx_slots;
    uint8_t rx_head;
    uint8_t rx_count;
    uint8_t nak_sent;         /* NAK already sent for the current rcv_nxt */
    uint8_t zero_advertised;  /* last advertised credits were 0 */

    uint32_t tx_frames;
    uint32_t retransmits;
    uint32_t dup_drops;
    uint32_t overflows;
    uint32_t unknown_type;
    uint32_t link_downs;
} fl_link;

/** Validate `cfg` and prepare the link. The handshake starts on the first fl_poll. */
fl_err fl_init(fl_link *link, const fl_config *cfg);

/**
 * Drive the link: read bytes through `read_bytes`, run timers, and send
 * HELLO, retransmissions and probes when they are due. Call it often, with a
 * millisecond clock. The clock may wrap around.
 */
void fl_poll(fl_link *link, uint32_t now_ms);

/** Feed received bytes, for callers that do not provide `read_bytes`. */
void fl_input(fl_link *link, const uint8_t *data, size_t len);

/**
 * Send one data frame. `type` must be 0x00..0xEF. Returns FL_EBUSY when the
 * window is full or the peer has no free slots. Try again after a later poll.
 * `seq_out`, if not NULL, receives the SEQ of the frame, for matching
 * FL_EV_DELIVERED events.
 */
fl_err fl_send(fl_link *link, uint8_t type, const uint8_t *payload, size_t len,
               uint8_t *seq_out);

/**
 * Take the oldest received frame from the queue. Returns FL_EAGAIN when the
 * queue is empty, and FL_ENOSPC, leaving the frame queued, when `cap` is too
 * small. Each successful call frees a slot, which becomes a credit for the peer.
 */
fl_err fl_recv(fl_link *link, uint8_t *type, uint8_t *buf, size_t cap, size_t *len);

/** Send a PING with up to 8 payload bytes. The answer arrives as FL_EV_PONG. */
fl_err fl_ping(fl_link *link, const uint8_t *payload, size_t len);

/** True once the handshake has completed and until the link goes down. */
int fl_is_connected(const fl_link *link);

/** Frames waiting in the receive queue. */
size_t fl_rx_pending(const fl_link *link);

/** Data frames sent but not yet acknowledged. */
size_t fl_tx_inflight(const fl_link *link);

/** Copy all counters into `out`. */
void fl_get_stats(const fl_link *link, fl_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* FRAMELINK_H */
