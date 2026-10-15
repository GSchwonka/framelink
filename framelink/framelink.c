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
