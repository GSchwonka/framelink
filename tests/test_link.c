/*
 * Unit tests for the framelink link layer: handshake, ARQ and flow control.
 *
 * Two endpoints, A and B, talk over a simulated wire. Each direction can
 * drop whole frames, flip random bytes, be cut completely, or drop frames
 * of one specific type. Time advances in 1 ms ticks, and both endpoints are
 * polled every tick.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#include <string.h>

#include "framelink.h"
#include "unity.h"

/* ------------------------------------------------------------------------ */
/* Simulated endpoints and wire                                             */
/* ------------------------------------------------------------------------ */

#define MAX_WINDOW 127u
#define MAX_SLOTS 127u
#define WIRE_CAP (1u << 16)

typedef struct endpoint {
    fl_link link;
    fl_config cfg;
    uint8_t tx[FL_TX_BUF_SIZE(MAX_WINDOW)];
    uint8_t rx[FL_RX_BUF_SIZE(MAX_SLOTS)];

    /* Bytes written by this endpoint, waiting to reach the peer. */
    uint8_t wire[WIRE_CAP];
    size_t wire_len;

    /* Faults applied to frames this endpoint writes. */
    unsigned drop_per_mille;    /* drop whole frames */
    unsigned corrupt_per_mille; /* flip one bit in a byte */
    int cut;                    /* drop everything */
    int drop_type;              /* drop frames of this TYPE ... */
    unsigned drop_type_count;   /* ... this many times, then stop */
    int drop_type_len;          /* only frames with this LEN, -1 for any */

    /* What this endpoint wrote, by type. */
    unsigned written[256];
    unsigned written_empty_credit;
    uint8_t last_hello[32];
    size_t last_hello_len;

    /* Events. */
    unsigned events[8];
    uint8_t last_event_arg[8];
    unsigned delivered;
} endpoint;

static endpoint A, B;
static uint32_t now;
static uint32_t rng_state;

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int ep_write(void *user, const uint8_t *data, size_t len)
{
    endpoint *e = (endpoint *)user;
    size_t i;
    uint8_t type = len > 2 ? data[2] : 0;

    e->written[type]++;
    if (type == FL_TYPE_CREDIT && data[1] == 0) {
        e->written_empty_credit++;
    }
    if (type == FL_TYPE_HELLO && len <= sizeof e->last_hello) {
        memcpy(e->last_hello, data, len);
        e->last_hello_len = len;
    }

    if (e->cut) {
        return (int)len;
    }
    if (e->drop_type >= 0 && type == (uint8_t)e->drop_type && e->drop_type_count > 0 &&
        (e->drop_type_len < 0 || data[1] == (uint8_t)e->drop_type_len)) {
        e->drop_type_count--;
        return (int)len;
    }
    if (e->drop_per_mille && rng() % 1000 < e->drop_per_mille) {
        return (int)len;
    }
    TEST_ASSERT_TRUE_MESSAGE(e->wire_len + len <= WIRE_CAP, "simulated wire overflow");
    for (i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (e->corrupt_per_mille && rng() % 1000 < e->corrupt_per_mille) {
            b ^= (uint8_t)(1u << (rng() % 8));
        }
        e->wire[e->wire_len++] = b;
    }
    return (int)len;
}

static void ep_event(void *user, fl_event ev, uint8_t arg)
{
    endpoint *e = (endpoint *)user;
    e->events[ev]++;
    e->last_event_arg[ev] = arg;
    if (ev == FL_EV_DELIVERED) {
        e->delivered++;
    }
}

static void ep_setup(endpoint *e, uint8_t window, uint8_t slots, uint16_t rto, uint8_t retries, uint8_t seed)
{
    memset(e, 0, sizeof *e);
    e->drop_type = -1;
    e->drop_type_len = -1;
    e->cfg.tx_buf = e->tx;
    e->cfg.tx_buf_len = FL_TX_BUF_SIZE(window);
    e->cfg.rx_buf = e->rx;
    e->cfg.rx_buf_len = FL_RX_BUF_SIZE(slots);
    e->cfg.window = window;
    e->cfg.max_retries = retries;
    e->cfg.rto_ms = rto;
    e->cfg.session_seed = seed;
    e->cfg.write_bytes = ep_write;
    e->cfg.on_event = ep_event;
    e->cfg.user = e;
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_init(&e->link, &e->cfg));
}

/* Move bytes across the wire in both directions, then poll both ends. */
static void tick(void)
{
    if (A.wire_len) {
        size_t n = A.wire_len;
        A.wire_len = 0;
        fl_input(&B.link, A.wire, n);
    }
    if (B.wire_len) {
        size_t n = B.wire_len;
        B.wire_len = 0;
        fl_input(&A.link, B.wire, n);
    }
    now++;
    fl_poll(&A.link, now);
    fl_poll(&B.link, now);
}

static void run(unsigned ms)
{
    while (ms--) {
        tick();
    }
}

static void connect_both(void)
{
    unsigned t;
    for (t = 0; t < 1000 && !(fl_is_connected(&A.link) && fl_is_connected(&B.link)); t++) {
        tick();
    }
    TEST_ASSERT_TRUE_MESSAGE(fl_is_connected(&A.link), "A did not connect");
    TEST_ASSERT_TRUE_MESSAGE(fl_is_connected(&B.link), "B did not connect");
}

/* Payload for frame i: its index, then i % 23 filler bytes. */
static size_t make_payload(uint32_t i, uint8_t *p)
{
    size_t len = 4 + i % 23, k;
    p[0] = (uint8_t)i;
    p[1] = (uint8_t)(i >> 8);
    p[2] = (uint8_t)(i >> 16);
    p[3] = (uint8_t)(i >> 24);
    for (k = 4; k < len; k++) {
        p[k] = (uint8_t)(i * 31u + k);
    }
    return len;
}

typedef struct transfer_result {
    unsigned ms;
    unsigned busy;
} transfer_result;

/* Send `count` frames from A to B. B reads one frame every `read_every` ms,
 * or everything waiting when it is 0. Checks that B sees every frame exactly
 * once and in order. */
static transfer_result transfer(uint32_t count, unsigned read_every, unsigned max_ms)
{
    transfer_result r = {0, 0};
    uint32_t sent = 0, received = 0;

    while (received < count) {
        uint8_t p[FL_MAX_PAYLOAD], q[FL_MAX_PAYLOAD], type;
        size_t len, qlen;
        fl_err err;

        TEST_ASSERT_TRUE_MESSAGE(r.ms < max_ms, "transfer did not finish in time");
        TEST_ASSERT_TRUE_MESSAGE(fl_is_connected(&A.link), "link went down during transfer");

        while (sent < count) {
            len = make_payload(sent, p);
            err = fl_send(&A.link, 0x01, p, len, NULL);
            if (err == FL_EBUSY) {
                r.busy++;
                break;
            }
            TEST_ASSERT_EQUAL_INT(FL_OK, err);
            sent++;
        }

        if (read_every == 0 || r.ms % read_every == 0) {
            while (fl_recv(&B.link, &type, q, sizeof q, &qlen) == FL_OK) {
                len = make_payload(received, p);
                TEST_ASSERT_EQUAL_HEX8(0x01, type);
                TEST_ASSERT_EQUAL_UINT_MESSAGE(len, qlen, "frame out of order or corrupted");
                TEST_ASSERT_EQUAL_HEX8_ARRAY(p, q, len);
                received++;
                if (read_every != 0) {
                    break;
                }
            }
        }
        tick();
        r.ms++;
    }
    return r;
}

static void finish(void)
{
    /* Let the last ACKs arrive, then check nothing is left in flight. */
    run(200);
    TEST_ASSERT_EQUAL_UINT(0, fl_tx_inflight(&A.link));
}

void setUp(void)
{
    now = 0;
    rng_state = 0x9E3779B9u;
    ep_setup(&A, 8, 8, 50, 10, 0x11);
    ep_setup(&B, 8, 8, 50, 10, 0x22);
}

void tearDown(void) {}

/* ------------------------------------------------------------------------ */
/* Configuration and API errors                                             */
/* ------------------------------------------------------------------------ */

static void test_init_rejects_invalid_config(void)
{
    fl_link l;
    fl_config c = A.cfg;

    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_init(NULL, &c));
    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_init(&l, NULL));
    c = A.cfg; c.window = 0;
    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_init(&l, &c));
    c = A.cfg; c.window = 128; c.tx_buf_len = sizeof A.tx;
    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_init(&l, &c));
    c = A.cfg; c.tx_buf_len = FL_TX_BUF_SIZE(8) - 1;
    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_init(&l, &c));
    c = A.cfg; c.rx_buf_len = FL_RX_SLOT_SIZE - 1;
    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_init(&l, &c));
    c = A.cfg; c.rto_ms = 0;
    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_init(&l, &c));
    c = A.cfg; c.max_retries = 0;
    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_init(&l, &c));
    c = A.cfg; c.write_bytes = NULL;
    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_init(&l, &c));
    c = A.cfg; c.window = 127; c.tx_buf_len = FL_TX_BUF_SIZE(127);
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_init(&l, &c));
}

static void test_send_and_recv_errors(void)
{
    uint8_t p[4] = {1, 2, 3, 4}, q[4], type;
    size_t len = 99;
    uint8_t big[256] = {0};
    unsigned i;

    TEST_ASSERT_EQUAL_INT(FL_ENOTCONN, fl_send(&A.link, 0x01, p, 4, NULL));
    connect_both();
    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_send(&A.link, FL_TYPE_ACK, p, 4, NULL));
    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_send(&A.link, 0x01, NULL, 4, NULL));
    TEST_ASSERT_EQUAL_INT(FL_ETOOBIG, fl_send(&A.link, 0x01, big, FL_MAX_PAYLOAD + 1u, NULL));
    TEST_ASSERT_EQUAL_INT(FL_EAGAIN, fl_recv(&B.link, &type, q, sizeof q, &len));

    /* Fill the window without letting anything cross the wire. */
    for (i = 0; i < 8; i++) {
        uint8_t seq = 0xAA;
        TEST_ASSERT_EQUAL_INT(FL_OK, fl_send(&A.link, 0x01, p, 4, &seq));
        TEST_ASSERT_EQUAL_UINT8(i, seq);
    }
    TEST_ASSERT_EQUAL_INT(FL_EBUSY, fl_send(&A.link, 0x01, p, 4, NULL));
    TEST_ASSERT_EQUAL_UINT(8, fl_tx_inflight(&A.link));

    tick();
    TEST_ASSERT_EQUAL_UINT(8, fl_rx_pending(&B.link));
    TEST_ASSERT_EQUAL_INT(FL_ENOSPC, fl_recv(&B.link, &type, q, 3, &len));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(8, fl_rx_pending(&B.link), "frame must stay queued on ENOSPC");
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_recv(&B.link, &type, q, sizeof q, &len));
    TEST_ASSERT_EQUAL_UINT(4, len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(p, q, 4);
}

/* ------------------------------------------------------------------------ */
/* Handshake                                                                */
/* ------------------------------------------------------------------------ */

static void test_handshake_simultaneous_start(void)
{
    connect_both();
    TEST_ASSERT_EQUAL_UINT(1, A.events[FL_EV_CONNECTED]);
    TEST_ASSERT_EQUAL_UINT(1, B.events[FL_EV_CONNECTED]);
    run(500);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, A.events[FL_EV_CONNECTED], "late HELLOs must not reconnect");
    TEST_ASSERT_EQUAL_UINT(0, A.events[FL_EV_PEER_RESET]);
    TEST_ASSERT_EQUAL_UINT(0, B.events[FL_EV_PEER_RESET]);
}

static void test_handshake_when_peer_starts_late(void)
{
    unsigned t;
    /* B is powered off: everything A sends is lost, and B is not polled. */
    for (t = 0; t < 300; t++) {
        A.wire_len = 0;
        now++;
        fl_poll(&A.link, now);
    }
    TEST_ASSERT_TRUE(A.written[FL_TYPE_HELLO] >= 5);
    TEST_ASSERT_FALSE(fl_is_connected(&A.link));
    connect_both();
    TEST_ASSERT_EQUAL_UINT(1, A.events[FL_EV_CONNECTED]);
    TEST_ASSERT_EQUAL_UINT(1, B.events[FL_EV_CONNECTED]);
}

static void test_handshake_survives_lost_hello_ack(void)
{
    B.drop_type = FL_TYPE_HELLO_ACK;
    B.drop_type_count = 3;
    A.drop_type = FL_TYPE_HELLO_ACK;
    A.drop_type_count = 3;
    connect_both();
    transfer(50, 0, 2000);
    finish();
}

/* A HELLO retransmission that arrives after the link is up must not reset
 * it. This is what the session ID in HELLO is for. */
static void test_duplicate_hello_does_not_reset_link(void)
{
    connect_both();
    transfer(20, 0, 2000);
    TEST_ASSERT_TRUE(A.last_hello_len > 0);
    fl_input(&B.link, A.last_hello, A.last_hello_len);
    transfer(20, 0, 2000);
    finish();
    TEST_ASSERT_EQUAL_UINT(0, B.events[FL_EV_PEER_RESET]);
    TEST_ASSERT_EQUAL_UINT(1, B.events[FL_EV_CONNECTED]);
    TEST_ASSERT_EQUAL_UINT(40, A.delivered);
}

static void test_version_mismatch_is_reported(void)
{
    uint8_t payload[5] = {2, 8, 8, 0, 0x77};
    uint8_t frame[32];
    size_t n;
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_encode(FL_TYPE_HELLO, 0, payload, 5, frame, sizeof frame, &n));
    fl_input(&A.link, frame, n);
    TEST_ASSERT_EQUAL_UINT(1, A.events[FL_EV_VERSION_MISMATCH]);
    TEST_ASSERT_EQUAL_UINT8(2, A.last_event_arg[FL_EV_VERSION_MISMATCH]);
    TEST_ASSERT_FALSE(fl_is_connected(&A.link));
}

/* ------------------------------------------------------------------------ */
/* Reliable delivery                                                        */
/* ------------------------------------------------------------------------ */

static void test_lossless_transfer(void)
{
    connect_both();
    transfer(1000, 0, 5000);
    finish();
    TEST_ASSERT_EQUAL_UINT(1000, A.delivered);
    TEST_ASSERT_EQUAL_UINT32(0, A.link.retransmits);
    TEST_ASSERT_EQUAL_UINT32(0, B.link.dup_drops);
}

/* Thousands of frames through the largest window: SEQ wraps many times and
 * the window spans the wrap most of the time. */
static void test_sequence_wraparound_with_max_window(void)
{
    ep_setup(&A, 127, 127, 50, 10, 0x11);
    ep_setup(&B, 127, 127, 50, 10, 0x22);
    connect_both();
    transfer(5000, 0, 20000);
    finish();
    TEST_ASSERT_EQUAL_UINT(5000, A.delivered);
}

static void test_lossy_channel_delivers_everything_in_order(void)
{
    static const unsigned drop[] = {10, 50, 100};
    static const unsigned corrupt[] = {1, 5, 10};
    unsigned i;
    for (i = 0; i < 3; i++) {
        setUp();
        ep_setup(&A, 8, 8, 30, 50, 0x11);
        ep_setup(&B, 8, 8, 30, 50, 0x22);
        connect_both();
        A.drop_per_mille = B.drop_per_mille = drop[i];
        A.corrupt_per_mille = B.corrupt_per_mille = corrupt[i];
        transfer(500, 0, 60000);
        A.drop_per_mille = B.drop_per_mille = 0;
        A.corrupt_per_mille = B.corrupt_per_mille = 0;
        finish();
        TEST_ASSERT_EQUAL_UINT(500, A.delivered);
        TEST_ASSERT_TRUE(A.link.retransmits > 0);
        TEST_ASSERT_EQUAL_UINT32(0, A.link.link_downs);
    }
}

/* Lost ACKs make the sender retransmit frames the receiver already has.
 * They must be dropped as duplicates, never delivered twice. */
static void test_lost_acks_cause_duplicates_not_double_delivery(void)
{
    connect_both();
    /* Fewer consecutive losses than max_retries: with all ACKs for one
     * frame lost more than max_retries times, link down is correct. */
    B.drop_type = FL_TYPE_ACK;
    B.drop_type_count = 12;
    transfer(100, 0, 5000);
    finish();
    TEST_ASSERT_TRUE(B.link.dup_drops > 0);
    TEST_ASSERT_EQUAL_UINT(100, A.delivered);
}

/* A data frame whose header is intact but whose body is corrupted is NAKed
 * at once through the decoder's corrupt hook, so it is resent long before
 * the retransmit timeout. No later frame exists to reveal the gap, so only
 * the hook can trigger this NAK. */
static void test_nak_recovers_faster_than_timeout(void)
{
    uint8_t p[16] = {0};
    ep_setup(&A, 8, 8, 2000, 10, 0x11);
    ep_setup(&B, 8, 8, 2000, 10, 0x22);
    connect_both();

    TEST_ASSERT_EQUAL_INT(FL_OK, fl_send(&A.link, 0x01, p, sizeof p, NULL));
    A.wire[A.wire_len - 3] ^= 0x01; /* a payload byte, header intact */
    run(10);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, B.written[FL_TYPE_NAK], "receiver must NAK the corrupted frame");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, fl_rx_pending(&B.link), "frame must arrive well before the 2 s timeout");
    TEST_ASSERT_EQUAL_UINT32(1, A.link.retransmits);
    finish();
}

/* ------------------------------------------------------------------------ */
/* Flow control                                                             */
/* ------------------------------------------------------------------------ */

static void test_slow_consumer_never_overflows(void)
{
    transfer_result r;
    ep_setup(&A, 16, 16, 50, 10, 0x11);
    ep_setup(&B, 16, 4, 50, 10, 0x22);
    connect_both();
    r = transfer(300, 7, 10000);
    finish();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, B.link.overflows, "credits must prevent overflow");
    TEST_ASSERT_EQUAL_UINT32(0, A.link.retransmits);
    TEST_ASSERT_TRUE_MESSAGE(r.busy > 0, "sender should have been held back by credits");
    TEST_ASSERT_EQUAL_UINT(300, A.delivered);
}

/* With credits ignored, the same slow consumer overflows. This is the
 * failure flow control exists to prevent. */
static void test_ignoring_credits_overflows(void)
{
    uint8_t p[8] = {0};
    unsigned i;
    ep_setup(&A, 16, 16, 50, 10, 0x11);
    ep_setup(&B, 16, 4, 50, 10, 0x22);
    connect_both();
    A.link.peer_credits = 255; /* a peer that lies about credits */
    for (i = 0; i < 12; i++) {
        TEST_ASSERT_EQUAL_INT(FL_OK, fl_send(&A.link, 0x01, p, sizeof p, NULL));
    }
    tick();
    TEST_ASSERT_TRUE(B.link.overflows > 0);
    TEST_ASSERT_EQUAL_UINT(4, fl_rx_pending(&B.link));
}

/* The receiver fills up, the application drains it, and the CREDIT update
 * announcing free slots is lost. The sender must recover with a probe. */
static void test_lost_credit_update_recovered_by_probe(void)
{
    uint8_t p[8] = {0}, q[8], type;
    size_t len;
    unsigned i;
    ep_setup(&A, 8, 8, 50, 10, 0x11);
    ep_setup(&B, 8, 2, 50, 10, 0x22);
    connect_both();

    TEST_ASSERT_EQUAL_INT(FL_OK, fl_send(&A.link, 0x01, p, sizeof p, NULL));
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_send(&A.link, 0x01, p, sizeof p, NULL));
    run(5);
    TEST_ASSERT_EQUAL_UINT(0, fl_tx_inflight(&A.link));
    TEST_ASSERT_EQUAL_INT(FL_EBUSY, fl_send(&A.link, 0x01, p, sizeof p, NULL));

    B.drop_type = FL_TYPE_CREDIT;
    B.drop_type_count = 1;
    B.drop_type_len = 2;
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_recv(&B.link, &type, q, sizeof q, &len));
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_recv(&B.link, &type, q, sizeof q, &len));
    TEST_ASSERT_EQUAL_UINT(1, B.written[FL_TYPE_CREDIT]);

    for (i = 0; i < 200 && fl_send(&A.link, 0x01, p, sizeof p, NULL) == FL_EBUSY; i++) {
        tick();
    }
    TEST_ASSERT_TRUE_MESSAGE(i < 200, "sender never recovered from the lost credit update");
    TEST_ASSERT_TRUE(A.written_empty_credit >= 1);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, A.link.link_downs, "probing must not count as retries");
}

/* A receiver that stays full for a long time is alive, not dead. */
static void test_long_stall_does_not_drop_link(void)
{
    uint8_t p[8] = {0};
    ep_setup(&A, 8, 8, 20, 3, 0x11);
    ep_setup(&B, 8, 2, 20, 3, 0x22);
    connect_both();
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_send(&A.link, 0x01, p, sizeof p, NULL));
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_send(&A.link, 0x01, p, sizeof p, NULL));
    run(5);
    TEST_ASSERT_EQUAL_INT(FL_EBUSY, fl_send(&A.link, 0x01, p, sizeof p, NULL));
    run(2000); /* 100 retransmit timeouts */
    TEST_ASSERT_TRUE(fl_is_connected(&A.link));
    TEST_ASSERT_EQUAL_UINT32(0, A.link.link_downs);
    TEST_ASSERT_TRUE(A.written_empty_credit > 50);
}

/* ------------------------------------------------------------------------ */
/* Failure and recovery                                                     */
/* ------------------------------------------------------------------------ */

static void test_link_down_reports_lost_frames_and_reconnects(void)
{
    uint8_t p[8] = {0};
    connect_both();
    A.cut = 1;
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_send(&A.link, 0x01, p, sizeof p, NULL));
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_send(&A.link, 0x01, p, sizeof p, NULL));
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_send(&A.link, 0x01, p, sizeof p, NULL));
    run(50u * 12u);
    TEST_ASSERT_EQUAL_UINT(1, A.events[FL_EV_LINK_DOWN]);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, A.last_event_arg[FL_EV_LINK_DOWN], "all three frames were lost");
    TEST_ASSERT_FALSE(fl_is_connected(&A.link));
    TEST_ASSERT_EQUAL_UINT32(1, A.link.link_downs);

    A.cut = 0;
    connect_both();
    TEST_ASSERT_EQUAL_UINT(2, A.events[FL_EV_CONNECTED]);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, B.events[FL_EV_PEER_RESET], "B must notice the new session");
    transfer(100, 0, 5000);
    finish();
}

static void test_peer_restart_is_detected(void)
{
    connect_both();
    transfer(30, 0, 2000);
    ep_setup(&B, 8, 8, 50, 10, 0x23); /* B reboots with a new session seed */
    connect_both();
    TEST_ASSERT_EQUAL_UINT(1, A.events[FL_EV_PEER_RESET]);
    TEST_ASSERT_EQUAL_UINT(2, A.events[FL_EV_CONNECTED]);
    transfer(30, 0, 2000);
    finish();
}

static void test_stale_ack_is_ignored(void)
{
    uint8_t p[8] = {0}, ack[2], frame[16];
    size_t n;
    connect_both();
    A.cut = 1;
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_send(&A.link, 0x01, p, sizeof p, NULL));
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_send(&A.link, 0x01, p, sizeof p, NULL));
    /* ACK for SEQ 200, far outside what is in flight, with huge credits. */
    ack[0] = 200;
    ack[1] = 255;
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_encode(FL_TYPE_ACK, 0, ack, 2, frame, sizeof frame, &n));
    fl_input(&A.link, frame, n);
    TEST_ASSERT_EQUAL_UINT(2, fl_tx_inflight(&A.link));
    TEST_ASSERT_EQUAL_UINT8(8, A.link.peer_credits);
    TEST_ASSERT_EQUAL_UINT(0, A.delivered);
}

static void test_ping_pong(void)
{
    uint8_t p[3] = {1, 2, 3};
    uint8_t big[9] = {0};
    connect_both();
    TEST_ASSERT_EQUAL_INT(FL_EINVAL, fl_ping(&A.link, big, sizeof big));
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_ping(&A.link, p, sizeof p));
    run(3);
    TEST_ASSERT_EQUAL_UINT(1, A.events[FL_EV_PONG]);
    TEST_ASSERT_EQUAL_UINT8(3, A.last_event_arg[FL_EV_PONG]);
}

static void test_reserved_types_are_counted(void)
{
    uint8_t frame[16];
    size_t n;
    TEST_ASSERT_EQUAL_INT(FL_OK, fl_encode(0xF9, 0, NULL, 0, frame, sizeof frame, &n));
    fl_input(&A.link, frame, n);
    TEST_ASSERT_EQUAL_UINT32(1, A.link.unknown_type);
}

static void test_clock_wraparound(void)
{
    now = 0xFFFFFF00u;
    A.drop_per_mille = B.drop_per_mille = 30;
    connect_both();
    transfer(300, 0, 20000);
    A.drop_per_mille = B.drop_per_mille = 0;
    finish();
    TEST_ASSERT_TRUE_MESSAGE(now < 0xFFFFFF00u, "test must cross the wrap");
    TEST_ASSERT_EQUAL_UINT32(0, A.link.link_downs);
}

static void test_stats_snapshot(void)
{
    fl_stats s;
    connect_both();
    transfer(10, 0, 1000);
    finish();
    fl_get_stats(&B.link, &s);
    TEST_ASSERT_EQUAL_UINT32(B.link.dec.stats.rx_frames, s.rx_frames);
    TEST_ASSERT_TRUE(s.rx_frames >= 11); /* HELLO or HELLO_ACK, plus 10 data */
    TEST_ASSERT_TRUE(s.tx_frames >= 11); /* HELLO_ACK or HELLO, plus 10 ACKs */
    TEST_ASSERT_EQUAL_UINT32(0, s.overflows);
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_invalid_config);
    RUN_TEST(test_send_and_recv_errors);
    RUN_TEST(test_handshake_simultaneous_start);
    RUN_TEST(test_handshake_when_peer_starts_late);
    RUN_TEST(test_handshake_survives_lost_hello_ack);
    RUN_TEST(test_duplicate_hello_does_not_reset_link);
    RUN_TEST(test_version_mismatch_is_reported);
    RUN_TEST(test_lossless_transfer);
    RUN_TEST(test_sequence_wraparound_with_max_window);
    RUN_TEST(test_lossy_channel_delivers_everything_in_order);
    RUN_TEST(test_lost_acks_cause_duplicates_not_double_delivery);
    RUN_TEST(test_nak_recovers_faster_than_timeout);
    RUN_TEST(test_slow_consumer_never_overflows);
    RUN_TEST(test_ignoring_credits_overflows);
    RUN_TEST(test_lost_credit_update_recovered_by_probe);
    RUN_TEST(test_long_stall_does_not_drop_link);
    RUN_TEST(test_link_down_reports_lost_frames_and_reconnects);
    RUN_TEST(test_peer_restart_is_detected);
    RUN_TEST(test_stale_ack_is_ignored);
    RUN_TEST(test_ping_pong);
    RUN_TEST(test_reserved_types_are_counted);
    RUN_TEST(test_clock_wraparound);
    RUN_TEST(test_stats_snapshot);
    return UNITY_END();
}
