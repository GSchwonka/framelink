/*
 * framelink - reliable framed messaging over unreliable byte streams.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 *
 * See docs/SPEC.md for the wire format and docs/DESIGN.md for the reasoning
 * behind it. Section numbers in comments refer to SPEC.md.
 */
#include "framelink.h"

/* ======================================================================== */
/* Internal helpers                                                         */
/* ======================================================================== */

/* Small byte copies without depending on <string.h>, so the library also
 * builds in freestanding environments. */
static void fl_copy(uint8_t *dst, const uint8_t *src, size_t n)
{
    while (n--) {
        *dst++ = *src++;
    }
}

/* ======================================================================== */
/* Checksums (section 3)                                                    */
/* ======================================================================== */

uint8_t fl_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00u;
    while (len--) {
        unsigned bit;
        crc = (uint8_t)(crc ^ *data++);
        for (bit = 0; bit < 8u; bit++) {
            crc = (uint8_t)((crc & 0x80u) ? ((unsigned)crc << 1) ^ 0x07u
                                          : ((unsigned)crc << 1));
        }
    }
    return crc;
}

#ifdef FL_CRC_TABLE
/* Generated for polynomial 0x1021. Entry i is the CRC register after
 * shifting the byte i through an all-zero register. */
static const uint16_t fl_crc16_table[256] = {
    0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50A5u, 0x60C6u, 0x70E7u,
    0x8108u, 0x9129u, 0xA14Au, 0xB16Bu, 0xC18Cu, 0xD1ADu, 0xE1CEu, 0xF1EFu,
    0x1231u, 0x0210u, 0x3273u, 0x2252u, 0x52B5u, 0x4294u, 0x72F7u, 0x62D6u,
    0x9339u, 0x8318u, 0xB37Bu, 0xA35Au, 0xD3BDu, 0xC39Cu, 0xF3FFu, 0xE3DEu,
    0x2462u, 0x3443u, 0x0420u, 0x1401u, 0x64E6u, 0x74C7u, 0x44A4u, 0x5485u,
    0xA56Au, 0xB54Bu, 0x8528u, 0x9509u, 0xE5EEu, 0xF5CFu, 0xC5ACu, 0xD58Du,
    0x3653u, 0x2672u, 0x1611u, 0x0630u, 0x76D7u, 0x66F6u, 0x5695u, 0x46B4u,
    0xB75Bu, 0xA77Au, 0x9719u, 0x8738u, 0xF7DFu, 0xE7FEu, 0xD79Du, 0xC7BCu,
    0x48C4u, 0x58E5u, 0x6886u, 0x78A7u, 0x0840u, 0x1861u, 0x2802u, 0x3823u,
    0xC9CCu, 0xD9EDu, 0xE98Eu, 0xF9AFu, 0x8948u, 0x9969u, 0xA90Au, 0xB92Bu,
    0x5AF5u, 0x4AD4u, 0x7AB7u, 0x6A96u, 0x1A71u, 0x0A50u, 0x3A33u, 0x2A12u,
    0xDBFDu, 0xCBDCu, 0xFBBFu, 0xEB9Eu, 0x9B79u, 0x8B58u, 0xBB3Bu, 0xAB1Au,
    0x6CA6u, 0x7C87u, 0x4CE4u, 0x5CC5u, 0x2C22u, 0x3C03u, 0x0C60u, 0x1C41u,
    0xEDAEu, 0xFD8Fu, 0xCDECu, 0xDDCDu, 0xAD2Au, 0xBD0Bu, 0x8D68u, 0x9D49u,
    0x7E97u, 0x6EB6u, 0x5ED5u, 0x4EF4u, 0x3E13u, 0x2E32u, 0x1E51u, 0x0E70u,
    0xFF9Fu, 0xEFBEu, 0xDFDDu, 0xCFFCu, 0xBF1Bu, 0xAF3Au, 0x9F59u, 0x8F78u,
    0x9188u, 0x81A9u, 0xB1CAu, 0xA1EBu, 0xD10Cu, 0xC12Du, 0xF14Eu, 0xE16Fu,
    0x1080u, 0x00A1u, 0x30C2u, 0x20E3u, 0x5004u, 0x4025u, 0x7046u, 0x6067u,
    0x83B9u, 0x9398u, 0xA3FBu, 0xB3DAu, 0xC33Du, 0xD31Cu, 0xE37Fu, 0xF35Eu,
    0x02B1u, 0x1290u, 0x22F3u, 0x32D2u, 0x4235u, 0x5214u, 0x6277u, 0x7256u,
    0xB5EAu, 0xA5CBu, 0x95A8u, 0x8589u, 0xF56Eu, 0xE54Fu, 0xD52Cu, 0xC50Du,
    0x34E2u, 0x24C3u, 0x14A0u, 0x0481u, 0x7466u, 0x6447u, 0x5424u, 0x4405u,
    0xA7DBu, 0xB7FAu, 0x8799u, 0x97B8u, 0xE75Fu, 0xF77Eu, 0xC71Du, 0xD73Cu,
    0x26D3u, 0x36F2u, 0x0691u, 0x16B0u, 0x6657u, 0x7676u, 0x4615u, 0x5634u,
    0xD94Cu, 0xC96Du, 0xF90Eu, 0xE92Fu, 0x99C8u, 0x89E9u, 0xB98Au, 0xA9ABu,
    0x5844u, 0x4865u, 0x7806u, 0x6827u, 0x18C0u, 0x08E1u, 0x3882u, 0x28A3u,
    0xCB7Du, 0xDB5Cu, 0xEB3Fu, 0xFB1Eu, 0x8BF9u, 0x9BD8u, 0xABBBu, 0xBB9Au,
    0x4A75u, 0x5A54u, 0x6A37u, 0x7A16u, 0x0AF1u, 0x1AD0u, 0x2AB3u, 0x3A92u,
    0xFD2Eu, 0xED0Fu, 0xDD6Cu, 0xCD4Du, 0xBDAAu, 0xAD8Bu, 0x9DE8u, 0x8DC9u,
    0x7C26u, 0x6C07u, 0x5C64u, 0x4C45u, 0x3CA2u, 0x2C83u, 0x1CE0u, 0x0CC1u,
    0xEF1Fu, 0xFF3Eu, 0xCF5Du, 0xDF7Cu, 0xAF9Bu, 0xBFBAu, 0x8FD9u, 0x9FF8u,
    0x6E17u, 0x7E36u, 0x4E55u, 0x5E74u, 0x2E93u, 0x3EB2u, 0x0ED1u, 0x1EF0u
};
#endif

uint16_t fl_crc16_update(uint16_t crc, uint8_t byte)
{
#ifdef FL_CRC_TABLE
    return (uint16_t)((uint16_t)(crc << 8) ^ fl_crc16_table[(uint8_t)((crc >> 8) ^ byte)]);
#else
    unsigned bit;
    crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)byte << 8));
    for (bit = 0; bit < 8u; bit++) {
        crc = (uint16_t)((crc & 0x8000u) ? ((unsigned)crc << 1) ^ 0x1021u
                                         : ((unsigned)crc << 1));
    }
    return crc;
#endif
}

uint16_t fl_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    while (len--) {
        crc = fl_crc16_update(crc, *data++);
    }
    return crc;
}

/* ======================================================================== */
/* Encoder (section 2)                                                      */
/* ======================================================================== */

fl_err fl_encode(uint8_t type, uint8_t seq,
                 const uint8_t *payload, size_t payload_len,
                 uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t frame_len;
    uint16_t crc;

    if (out == NULL || (payload_len > 0u && payload == NULL)) {
        return FL_EINVAL;
    }
    if (payload_len > (size_t)FL_MAX_PAYLOAD) {
        return FL_ETOOBIG;
    }
    frame_len = FL_FRAME_SIZE(payload_len);
    if (out_cap < frame_len) {
        return FL_ENOSPC;
    }

    out[0] = (uint8_t)FL_SYNC;
    out[1] = (uint8_t)payload_len;
    out[2] = type;
    out[3] = seq;
    out[4] = fl_crc8(&out[1], 3u);
    if (payload_len > 0u) {
        fl_copy(&out[FL_HEADER_LEN], payload, payload_len);
    }

    /* CRC16 covers LEN TYPE SEQ HCRC PAYLOAD, sent low byte first. */
    crc = fl_crc16(&out[1], 4u + payload_len);
    out[FL_HEADER_LEN + payload_len] = (uint8_t)(crc & 0xFFu);
    out[FL_HEADER_LEN + payload_len + 1u] = (uint8_t)(crc >> 8);

    if (out_len != NULL) {
        *out_len = frame_len;
    }
    return FL_OK;
}

/* ======================================================================== */
/* Decoder (section 6)                                                      */
/*                                                                          */
/* The decoder keeps the bytes of one candidate frame in `buf`. buf[0] is   */
/* always SYNC while n > 0. When a candidate fails either CRC, only its     */
/* first byte is dropped and the remaining bytes are scanned again, so a    */
/* valid frame that began inside the failed one is still found. The scan    */
/* is a loop, never recursion, and every iteration either waits for more   */
/* input or removes at least one byte, so it always terminates.             */
/* ======================================================================== */

/* Remove `count` bytes from the front of the candidate buffer. */
static void dec_consume(fl_decoder *dec, uint16_t count)
{
    uint16_t i;
    uint16_t rest = (uint16_t)(dec->n - count);
    for (i = 0; i < rest; i++) {
        dec->buf[i] = dec->buf[count + i];
    }
    dec->n = rest;
    dec->need = 0;
}

/* The candidate at buf[0] is invalid. Keep its bytes for rescanning. */
static void dec_reject(fl_decoder *dec)
{
    dec->resync_pending = 1;
    dec_consume(dec, 1);
}

static void dec_emit(fl_decoder *dec, fl_frame_cb on_frame, void *user)
{
    fl_frame frame;
    frame.len = dec->buf[1];
    frame.type = dec->buf[2];
    frame.seq = dec->buf[3];
    frame.payload = &dec->buf[FL_HEADER_LEN];

    dec->stats.rx_frames++;
    if (dec->resync_pending) {
        dec->stats.resyncs++;
        dec->resync_pending = 0;
    }
    if (on_frame != NULL) {
        on_frame(user, &frame);
    }
}

/* Advance the state machine as far as the buffered bytes allow. */
static void dec_process(fl_decoder *dec, fl_frame_cb on_frame, void *user)
{
    for (;;) {
        uint16_t crc_calc;
        uint16_t crc_recv;

        /* Realign on the next SYNC. Only happens after a rejection or after
         * a frame was emitted with bytes left over. */
        if (dec->n > 0u && dec->buf[0] != FL_SYNC) {
            uint16_t skip = 1;
            while (skip < dec->n && dec->buf[skip] != FL_SYNC) {
                skip++;
            }
            dec->stats.hunt_bytes += skip;
            dec_consume(dec, skip);
        }
        if (dec->n == 0u) {
            dec->resync_pending = 0;
            return;
        }

        /* Header: SYNC LEN TYPE SEQ HCRC. */
        if (dec->need == 0u) {
            if (dec->n < FL_HEADER_LEN) {
                return;
            }
            if (fl_crc8(&dec->buf[1], 3u) != dec->buf[4]) {
                dec->stats.hdr_errors++;
                dec_reject(dec);
                continue;
            }
#if FL_MAX_PAYLOAD < 255
            if (dec->buf[1] > FL_MAX_PAYLOAD) {
                dec->stats.hdr_errors++;
                dec_reject(dec);
                continue;
            }
#endif
            dec->need = (uint16_t)(dec->buf[1] + FL_OVERHEAD);
        }

        if (dec->n < dec->need) {
            return;
        }

        /* Whole frame buffered: check CRC16 over LEN..PAYLOAD. */
        crc_calc = fl_crc16(&dec->buf[1], (size_t)dec->need - 3u);
        crc_recv = (uint16_t)(dec->buf[dec->need - 2u] |
                              (uint16_t)(dec->buf[dec->need - 1u] << 8));
        if (crc_calc != crc_recv) {
            dec->stats.crc_errors++;
            if (dec->on_corrupt != NULL) {
                fl_frame bad;
                bad.len = dec->buf[1];
                bad.type = dec->buf[2];
                bad.seq = dec->buf[3];
                bad.payload = &dec->buf[FL_HEADER_LEN];
                dec->on_corrupt(dec->corrupt_user, &bad);
            }
            dec_reject(dec);
            continue;
        }

        dec_emit(dec, on_frame, user);
        dec_consume(dec, dec->need);
    }
}

void fl_decoder_init(fl_decoder *dec)
{
    uint8_t *p = (uint8_t *)dec;
    size_t i;
    for (i = 0; i < sizeof *dec; i++) {
        p[i] = 0;
    }
}

void fl_decoder_feed(fl_decoder *dec, const uint8_t *data, size_t len,
                     fl_frame_cb on_frame, void *user)
{
    while (len--) {
        uint8_t byte = *data++;
        if (dec->n == 0u && byte != FL_SYNC) {
            dec->stats.hunt_bytes++;
            continue;
        }
        /* Cannot overflow: after dec_process returns, n < need <= FL_MAX_FRAME
         * or, before the header is checked, n < FL_HEADER_LEN. */
        dec->buf[dec->n++] = byte;
        dec_process(dec, on_frame, user);
    }
}

void fl_decoder_timeout(fl_decoder *dec, fl_frame_cb on_frame, void *user)
{
    if (dec->n == 0u) {
        return;
    }
    dec->stats.timeouts++;
    /* No more bytes are coming for any buffered candidate. Reject them one
     * by one, delivering any complete frame found along the way. */
    while (dec->n > 0u) {
        dec_reject(dec);
        dec_process(dec, on_frame, user);
    }
}

int fl_decoder_busy(const fl_decoder *dec)
{
    return dec->n > 0u;
}

void fl_decoder_set_corrupt_cb(fl_decoder *dec, fl_frame_cb on_corrupt, void *user)
{
    dec->on_corrupt = on_corrupt;
    dec->corrupt_user = user;
}

/* ======================================================================== */
/* Link layer (section 5)                                                   */
/*                                                                          */
/* Sender: frames in flight live in tx_buf, one FL_MAX_FRAME slot each, in  */
/* a ring that starts at slot tx_head for SEQ snd_una. Slots are indexed by */
/* distance from snd_una, never by SEQ modulo window, which would collide   */
/* when SEQ wraps from 255 to 0 and 256 is not a multiple of the window.    */
/*                                                                          */
/* Receiver: accepted frames wait in rx_buf, a ring of FL_RX_SLOT_SIZE      */
/* slots holding LEN TYPE PAYLOAD. Free slots are the credits we advertise. */
/* ======================================================================== */

#define FL_PING_MAX 8u
#define FL_HELLO_LEN 5u
#define FL_HELLO_ACK_LEN 6u

/* True when `deadline` has been reached. Correct across clock wraparound as
 * long as deadlines are less than 2^31 ms in the future. */
static int fl_due(uint32_t now, uint32_t deadline)
{
    return (uint32_t)(now - deadline) < 0x80000000u;
}

static void fl_emit(fl_link *link, fl_event event, uint8_t arg)
{
    if (link->cfg.on_event != NULL) {
        link->cfg.on_event(link->cfg.user, event, arg);
    }
}

static uint8_t fl_inflight(const fl_link *link)
{
    return (uint8_t)(link->snd_nxt - link->snd_una);
}

static uint8_t fl_free_slots(const fl_link *link)
{
    return (uint8_t)(link->rx_slots - link->rx_count);
}

static void fl_write(fl_link *link, const uint8_t *frame, size_t len)
{
    (void)link->cfg.write_bytes(link->cfg.user, frame, len);
    link->tx_frames++;
}

static void fl_send_control(fl_link *link, uint8_t type, const uint8_t *payload, size_t len)
{
    uint8_t frame[FL_OVERHEAD + FL_HELLO_ACK_LEN + FL_PING_MAX];
    size_t n = 0;
    if (fl_encode(type, 0, payload, len, frame, sizeof frame, &n) == FL_OK) {
        fl_write(link, frame, n);
    }
}

/* ACK, NAK and CREDIT updates all carry the receiver position and credits. */
static void fl_send_position(fl_link *link, uint8_t type, uint8_t seq)
{
    uint8_t payload[2];
    payload[0] = seq;
    payload[1] = fl_free_slots(link);
    link->zero_advertised = (uint8_t)(payload[1] == 0u);
    fl_send_control(link, type, payload, sizeof payload);
}

static void fl_send_ack(fl_link *link)
{
    fl_send_position(link, FL_TYPE_ACK, (uint8_t)(link->rcv_nxt - 1u));
}

static void fl_send_hello(fl_link *link, uint8_t type, uint8_t echo)
{
    uint8_t payload[FL_HELLO_ACK_LEN];
    payload[0] = FL_PROTOCOL_VERSION;
    payload[1] = link->cfg.window;
    payload[2] = fl_free_slots(link);
    payload[3] = 0; /* flags */
    payload[4] = link->session;
    payload[5] = echo;
    link->zero_advertised = (uint8_t)(payload[2] == 0u);
    fl_send_control(link, type, payload, type == FL_TYPE_HELLO ? FL_HELLO_LEN : FL_HELLO_ACK_LEN);
}

static uint8_t *fl_tx_slot(fl_link *link, uint8_t offset)
{
    unsigned slot = ((unsigned)link->tx_head + offset) % link->cfg.window;
    return &link->cfg.tx_buf[slot * FL_MAX_FRAME];
}

static void fl_transmit(fl_link *link, uint8_t offset)
{
    const uint8_t *frame = fl_tx_slot(link, offset);
    fl_write(link, frame, FL_FRAME_SIZE(frame[1]));
}

/* Go-Back-N: resend everything from snd_una in order. */
static void fl_retransmit_all(fl_link *link)
{
    uint8_t i, n = fl_inflight(link);
    for (i = 0; i < n; i++) {
        fl_transmit(link, i);
        link->retransmits++;
    }
    link->timer_on = (uint8_t)(n > 0u);
    link->rto_due = link->now + link->cfg.rto_ms;
}

/* Forget sequence state in both directions. Frames already in the receive
 * queue stay there: they were delivered in order and the application may
 * still read them. Returns how many sent frames were never acknowledged. */
static uint8_t fl_reset_sequences(fl_link *link)
{
    uint8_t dropped = fl_inflight(link);
    link->snd_una = 0;
    link->snd_nxt = 0;
    link->tx_head = 0;
    link->peer_credits = 0;
    link->retries = 0;
    link->timer_on = 0;
    link->want_credit = 0;
    link->rcv_nxt = 0;
    link->nak_sent = 0;
    return dropped;
}

/* Section 5.3: retries exhausted. Start over with a new session ID. */
static void fl_link_down(fl_link *link)
{
    uint8_t dropped = fl_reset_sequences(link);
    link->connected = 0;
    link->session++;
    link->peer_session_valid = 0; /* accept any HELLO from the peer */
    link->hello_now = 1;
    link->link_downs++;
    fl_emit(link, FL_EV_LINK_DOWN, dropped);
}

static void fl_connect(fl_link *link, uint8_t peer_credits)
{
    link->connected = 1;
    link->peer_credits = peer_credits;
    fl_emit(link, FL_EV_CONNECTED, 0);
}

/* Section 5.2. */
static void fl_on_hello(fl_link *link, const fl_frame *f)
{
    uint8_t peer_session;
    if (f->len < FL_HELLO_LEN) {
        return;
    }
    if (f->payload[0] != FL_PROTOCOL_VERSION) {
        fl_emit(link, FL_EV_VERSION_MISMATCH, f->payload[0]);
        return;
    }
    peer_session = f->payload[4];

    if (link->peer_session_valid && peer_session == link->peer_session) {
        /* A retransmitted HELLO. Answer it, change nothing. */
        fl_send_hello(link, FL_TYPE_HELLO_ACK, peer_session);
        return;
    }

    /* A new session from the peer: whatever we had with it is gone. */
    {
        uint8_t was_connected = link->connected;
        uint8_t dropped = fl_reset_sequences(link);
        link->peer_session = peer_session;
        link->peer_session_valid = 1;
        fl_send_hello(link, FL_TYPE_HELLO_ACK, peer_session);
        if (was_connected) {
            fl_emit(link, FL_EV_PEER_RESET, dropped);
        }
        fl_connect(link, f->payload[2]);
    }
}

static void fl_on_hello_ack(fl_link *link, const fl_frame *f)
{
    if (f->len < FL_HELLO_ACK_LEN) {
        return;
    }
    if (f->payload[0] != FL_PROTOCOL_VERSION) {
        fl_emit(link, FL_EV_VERSION_MISMATCH, f->payload[0]);
        return;
    }
    if (f->payload[5] != link->session || link->connected) {
        return; /* stale, or a duplicate after we already connected */
    }
    link->peer_session = f->payload[4];
    link->peer_session_valid = 1;
    fl_connect(link, f->payload[2]);
}

/* Section 5.5: ACK, NAK and CREDIT updates. */
static void fl_on_position(fl_link *link, uint8_t ack_seq, uint8_t credits, int is_nak)
{
    uint8_t inflight = fl_inflight(link);
    uint8_t acked = (uint8_t)(ack_seq + 1u - link->snd_una);
    uint8_t first = link->snd_una;
    uint8_t i;

    if (acked > inflight) {
        return; /* stale or invalid: ignore it, credits included */
    }

    link->snd_una = (uint8_t)(link->snd_una + acked);
    link->tx_head = (uint8_t)(((unsigned)link->tx_head + acked) % link->cfg.window);
    link->peer_credits = credits;
    if (credits > 0u) {
        link->want_credit = 0;
    }
    if (acked > 0u) {
        link->retries = 0;
        link->timer_on = (uint8_t)(fl_inflight(link) > 0u);
        link->rto_due = link->now + link->cfg.rto_ms;
    }
    if (is_nak && fl_inflight(link) > 0u) {
        fl_retransmit_all(link);
    }

    /* Events last, so callbacks see consistent state and may call fl_send. */
    for (i = 0; i < acked; i++) {
        fl_emit(link, FL_EV_DELIVERED, (uint8_t)(first + i));
    }
}

/* Section 5.4. */
static void fl_on_data(fl_link *link, const fl_frame *f)
{
    uint8_t distance = (uint8_t)(f->seq - link->rcv_nxt);

    if (distance == 0u) {
        if (link->rx_count < link->rx_slots) {
            unsigned slot = ((unsigned)link->rx_head + link->rx_count) % link->rx_slots;
            uint8_t *dst = &link->cfg.rx_buf[slot * FL_RX_SLOT_SIZE];
            dst[0] = f->len;
            dst[1] = f->type;
            fl_copy(&dst[2], f->payload, f->len);
            link->rx_count++;
            link->rcv_nxt++;
            link->nak_sent = 0;
        } else {
            link->overflows++; /* the peer ignored our credits */
        }
        fl_send_ack(link);
    } else if (distance >= 128u) {
        link->dup_drops++; /* our ACK was probably lost: repeat it */
        fl_send_ack(link);
    } else if (!link->nak_sent) {
        link->nak_sent = 1; /* a gap: ask once for rcv_nxt */
        fl_send_position(link, FL_TYPE_NAK, link->rcv_nxt);
    }
}

static void fl_on_frame(void *user, const fl_frame *f)
{
    fl_link *link = (fl_link *)user;

    switch (f->type) {
    case FL_TYPE_HELLO:
        fl_on_hello(link, f);
        return;
    case FL_TYPE_HELLO_ACK:
        fl_on_hello_ack(link, f);
        return;
    case FL_TYPE_PING:
        if (f->len <= FL_PING_MAX) {
            fl_send_control(link, FL_TYPE_PONG, f->payload, f->len);
        }
        return;
    case FL_TYPE_PONG:
        fl_emit(link, FL_EV_PONG, f->len);
        return;
    default:
        break;
    }

    if (f->type > FL_TYPE_PONG) {
        link->unknown_type++;
        return;
    }
    if (!link->connected) {
        return; /* section 5.2 rule 7 */
    }

    switch (f->type) {
    case FL_TYPE_ACK:
    case FL_TYPE_NAK:
        if (f->len == 2u) {
            fl_on_position(link, (uint8_t)(f->type == FL_TYPE_NAK ? f->payload[0] - 1u : f->payload[0]),
                           f->payload[1], f->type == FL_TYPE_NAK);
        }
        return;
    case FL_TYPE_CREDIT:
        if (f->len == 0u) {
            fl_send_position(link, FL_TYPE_CREDIT, (uint8_t)(link->rcv_nxt - 1u));
        } else if (f->len == 2u) {
            fl_on_position(link, f->payload[0], f->payload[1], 0);
        }
        return;
    default:
        fl_on_data(link, f);
        return;
    }
}

/* Header passed, body failed: if it was the data frame we are waiting for,
 * ask for it now rather than after a full timeout. */
static void fl_on_corrupt(void *user, const fl_frame *f)
{
    fl_link *link = (fl_link *)user;
    if (link->connected && f->type <= FL_TYPE_USER_MAX && !link->nak_sent &&
        (uint8_t)(f->seq - link->rcv_nxt) < 128u) {
        link->nak_sent = 1;
        fl_send_position(link, FL_TYPE_NAK, link->rcv_nxt);
    }
}

fl_err fl_init(fl_link *link, const fl_config *cfg)
{
    size_t slots;
    uint8_t *p = (uint8_t *)link;
    size_t i;

    if (link == NULL || cfg == NULL || cfg->write_bytes == NULL ||
        cfg->tx_buf == NULL || cfg->rx_buf == NULL ||
        cfg->window == 0u || cfg->window > 127u ||
        cfg->rto_ms == 0u || cfg->max_retries == 0u ||
        cfg->tx_buf_len < FL_TX_BUF_SIZE(cfg->window)) {
        return FL_EINVAL;
    }
    slots = cfg->rx_buf_len / FL_RX_SLOT_SIZE;
    if (slots == 0u) {
        return FL_EINVAL;
    }

    for (i = 0; i < sizeof *link; i++) {
        p[i] = 0;
    }
    link->cfg = *cfg;
    link->rx_slots = (uint8_t)(slots > 255u ? 255u : slots);
    link->session = cfg->session_seed;
    link->hello_now = 1;
    fl_decoder_init(&link->dec);
    fl_decoder_set_corrupt_cb(&link->dec, fl_on_corrupt, link);
    return FL_OK;
}

void fl_input(fl_link *link, const uint8_t *data, size_t len)
{
    if (len > 0u) {
        link->last_rx = link->now;
        fl_decoder_feed(&link->dec, data, len, fl_on_frame, link);
    }
}

void fl_poll(fl_link *link, uint32_t now_ms)
{
    uint32_t frame_timeout = link->cfg.frame_timeout_ms ? link->cfg.frame_timeout_ms : link->cfg.rto_ms;
    link->now = now_ms;

    if (link->cfg.read_bytes != NULL) {
        uint8_t chunk[64];
        unsigned rounds;
        /* Bounded, so a flood of input cannot starve the timers below. */
        for (rounds = 0; rounds < 16u; rounds++) {
            size_t n = link->cfg.read_bytes(link->cfg.user, chunk, sizeof chunk);
            if (n == 0u) {
                break;
            }
            fl_input(link, chunk, n > sizeof chunk ? sizeof chunk : n);
        }
    }

    if (fl_decoder_busy(&link->dec) && fl_due(now_ms, link->last_rx + frame_timeout)) {
        fl_decoder_timeout(&link->dec, fl_on_frame, link);
    }

    if (!link->connected) {
        if (link->hello_now || fl_due(now_ms, link->hello_due)) {
            link->hello_now = 0;
            link->hello_due = now_ms + link->cfg.rto_ms;
            fl_send_hello(link, FL_TYPE_HELLO, 0);
        }
        return;
    }

    if (link->timer_on && fl_due(now_ms, link->rto_due)) {
        if (link->retries >= link->cfg.max_retries) {
            fl_link_down(link);
            return;
        }
        link->retries++;
        fl_retransmit_all(link);
    }

    /* Section 5.6: zero-window probe. Not counted as a retry. */
    if (link->want_credit && link->peer_credits == 0u && fl_inflight(link) == 0u &&
        fl_due(now_ms, link->probe_due)) {
        link->probe_due = now_ms + link->cfg.rto_ms;
        fl_send_control(link, FL_TYPE_CREDIT, NULL, 0);
    }
}

fl_err fl_send(fl_link *link, uint8_t type, const uint8_t *payload, size_t len,
               uint8_t *seq_out)
{
    uint8_t inflight;
    uint8_t *slot;

    if (type > FL_TYPE_USER_MAX || (len > 0u && payload == NULL)) {
        return FL_EINVAL;
    }
    if (len > (size_t)FL_MAX_PAYLOAD) {
        return FL_ETOOBIG;
    }
    if (!link->connected) {
        return FL_ENOTCONN;
    }
    inflight = fl_inflight(link);
    if (inflight >= link->cfg.window) {
        return FL_EBUSY;
    }
    if (inflight >= link->peer_credits) {
        if (!link->want_credit) {
            link->want_credit = 1;
            link->probe_due = link->now + link->cfg.rto_ms;
        }
        return FL_EBUSY;
    }

    slot = fl_tx_slot(link, inflight);
    (void)fl_encode(type, link->snd_nxt, payload, len, slot, FL_MAX_FRAME, NULL);
    if (seq_out != NULL) {
        *seq_out = link->snd_nxt;
    }
    link->snd_nxt++;
    fl_transmit(link, inflight);
    if (!link->timer_on) {
        link->timer_on = 1;
        link->rto_due = link->now + link->cfg.rto_ms;
    }
    return FL_OK;
}

fl_err fl_recv(fl_link *link, uint8_t *type, uint8_t *buf, size_t cap, size_t *len)
{
    const uint8_t *src;
    if (link->rx_count == 0u) {
        return FL_EAGAIN;
    }
    src = &link->cfg.rx_buf[(size_t)link->rx_head * FL_RX_SLOT_SIZE];
    if (cap < src[0] || (src[0] > 0u && buf == NULL)) {
        return FL_ENOSPC;
    }
    if (type != NULL) {
        *type = src[1];
    }
    if (len != NULL) {
        *len = src[0];
    }
    fl_copy(buf, &src[2], src[0]);
    link->rx_head = (uint8_t)(((unsigned)link->rx_head + 1u) % link->rx_slots);
    link->rx_count--;

    /* Section 5.6: the peer may be waiting for exactly this slot. */
    if (link->zero_advertised && link->connected) {
        fl_send_position(link, FL_TYPE_CREDIT, (uint8_t)(link->rcv_nxt - 1u));
    }
    return FL_OK;
}

fl_err fl_ping(fl_link *link, const uint8_t *payload, size_t len)
{
    if (len > FL_PING_MAX || (len > 0u && payload == NULL)) {
        return FL_EINVAL;
    }
    fl_send_control(link, FL_TYPE_PING, payload, len);
    return FL_OK;
}

int fl_is_connected(const fl_link *link)
{
    return link->connected != 0u;
}

size_t fl_rx_pending(const fl_link *link)
{
    return link->rx_count;
}

size_t fl_tx_inflight(const fl_link *link)
{
    return fl_inflight(link);
}

void fl_get_stats(const fl_link *link, fl_stats *out)
{
    out->rx_frames = link->dec.stats.rx_frames;
    out->tx_frames = link->tx_frames;
    out->hunt_bytes = link->dec.stats.hunt_bytes;
    out->hdr_errors = link->dec.stats.hdr_errors;
    out->crc_errors = link->dec.stats.crc_errors;
    out->resyncs = link->dec.stats.resyncs;
    out->timeouts = link->dec.stats.timeouts;
    out->retransmits = link->retransmits;
    out->dup_drops = link->dup_drops;
    out->overflows = link->overflows;
    out->unknown_type = link->unknown_type;
    out->link_down = link->link_downs;
}
