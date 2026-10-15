/*
 * fl_peer - a framelink endpoint over TCP that echoes every frame it receives.
 *
 * It is the C side of the interoperability tests: the Python host connects,
 * sends frames, and checks that each comes back unchanged and in order. It
 * also shows how small a complete framelink integration is.
 *
 *   fl_peer --listen PORT [options]
 *   fl_peer --connect HOST:PORT [options]
 *
 * Options:
 *   --window N         frames in flight (default 8)
 *   --slots N          receive queue slots (default 8)
 *   --rto MS           retransmit timeout (default 50)
 *   --retries N        retries before link down (default 10)
 *   --seed N           session seed (default: from the clock)
 *   --consume-every MS take one frame from the queue every MS milliseconds,
 *                      to act as a slow consumer (default 0: as fast as possible)
 *   --idle-exit MS     exit after MS without any received bytes (default 2000).
 *                      The peer also exits when the connection closes.
 *
 * On exit it prints its statistics as one JSON line on stdout.
 *
 * Builds on Linux, macOS and Windows (Winsock).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close close
#endif

#include "framelink.h"

#define MAX_WINDOW 64u
#define MAX_SLOTS 64u

static uint8_t tx_buf[FL_TX_BUF_SIZE(MAX_WINDOW)];
static uint8_t rx_buf[FL_RX_BUF_SIZE(MAX_SLOTS)];

typedef struct peer {
    sock_t sock;
    int closed;
    uint32_t last_rx;   /* when bytes last arrived */
    unsigned events[8];
} peer;

static uint32_t now_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount(); /* wraps every 49.7 days, like framelink's clock */
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
#endif
}

/* Wait until the socket is readable or `ms` passes. Returns 1 if readable. */
static int wait_readable(sock_t s, unsigned ms)
{
    fd_set set;
    struct timeval tv;
    FD_ZERO(&set);
    FD_SET(s, &set);
    tv.tv_sec = (long)(ms / 1000u);
    tv.tv_usec = (long)((ms % 1000u) * 1000u);
    return select((int)s + 1, &set, NULL, NULL, &tv) > 0;
}

static int on_write(void *user, const uint8_t *data, size_t len)
{
    peer *p = (peer *)user;
    size_t done = 0;
    while (done < len && !p->closed) {
        int n = send(p->sock, (const char *)data + done, (int)(len - done), 0);
        if (n <= 0) {
            p->closed = 1;
            break;
        }
        done += (size_t)n;
    }
    return (int)done;
}

static size_t on_read(void *user, uint8_t *data, size_t cap)
{
    peer *p = (peer *)user;
    int n;
    if (p->closed || !wait_readable(p->sock, 0)) {
        return 0;
    }
    n = recv(p->sock, (char *)data, (int)cap, 0);
    if (n <= 0) {
        p->closed = 1;
        return 0;
    }
    p->last_rx = now_ms();
    return (size_t)n;
}

static void on_event(void *user, fl_event ev, uint8_t arg)
{
    peer *p = (peer *)user;
    p->events[ev]++;
    if (ev == FL_EV_LINK_DOWN || ev == FL_EV_PEER_RESET) {
        fprintf(stderr, "fl_peer: %s, %u frame(s) unacknowledged\n",
                ev == FL_EV_LINK_DOWN ? "link down" : "peer reset", arg);
    }
}

/* IPv4 only, numeric addresses, which keeps this portable to every
 * toolchain. Use "127.0.0.1:PORT" with --connect. */
static sock_t open_socket(const char *listen_port, const char *connect_to)
{
    struct sockaddr_in addr;
    sock_t s;
    int one = 1;

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    if (connect_to != NULL) {
        char host[64];
        const char *colon = strrchr(connect_to, ':');
        size_t hl = colon ? (size_t)(colon - connect_to) : 0;
        if (colon == NULL || hl == 0 || hl >= sizeof host) {
            return SOCK_INVALID;
        }
        memcpy(host, connect_to, hl);
        host[hl] = '\0';
        addr.sin_addr.s_addr = inet_addr(host);
        addr.sin_port = htons((unsigned short)strtoul(colon + 1, NULL, 10));
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((unsigned short)strtoul(listen_port, NULL, 10));
    }

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == SOCK_INVALID) {
        return SOCK_INVALID;
    }
    if (connect_to != NULL) {
        if (connect(s, (struct sockaddr *)&addr, sizeof addr) != 0) {
            sock_close(s);
            return SOCK_INVALID;
        }
    } else {
        sock_t c;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
        if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(s, 1) != 0) {
            sock_close(s);
            return SOCK_INVALID;
        }
        fprintf(stderr, "fl_peer: listening on port %s\n", listen_port);
        fflush(stderr);
        c = accept(s, NULL, NULL);
        sock_close(s);
        s = c;
        if (s == SOCK_INVALID) {
            return SOCK_INVALID;
        }
    }
    /* Frames are small: send them now rather than waiting for more. */
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return s;
}

static unsigned long arg_num(const char *s)
{
    return strtoul(s, NULL, 0);
}

int main(int argc, char **argv)
{
    const char *listen_port = NULL, *connect_to = NULL;
    unsigned window = 8, slots = 8, rto = 50, retries = 10, consume_every = 0, idle_exit = 2000;
    unsigned long seed = 0;
    int have_seed = 0, i;
    peer p;
    fl_link link;
    fl_config cfg;
    fl_stats st;
    uint32_t last_consume;
    uint8_t held_type = 0, held[FL_MAX_PAYLOAD];
    size_t held_len = 0;
    int holding = 0;
    unsigned long echoed = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (v == NULL) {
            fprintf(stderr, "fl_peer: missing value for %s\n", a);
            return 2;
        }
        if (!strcmp(a, "--listen")) listen_port = v;
        else if (!strcmp(a, "--connect")) connect_to = v;
        else if (!strcmp(a, "--window")) window = (unsigned)arg_num(v);
        else if (!strcmp(a, "--slots")) slots = (unsigned)arg_num(v);
        else if (!strcmp(a, "--rto")) rto = (unsigned)arg_num(v);
        else if (!strcmp(a, "--retries")) retries = (unsigned)arg_num(v);
        else if (!strcmp(a, "--seed")) { seed = arg_num(v); have_seed = 1; }
        else if (!strcmp(a, "--consume-every")) consume_every = (unsigned)arg_num(v);
        else if (!strcmp(a, "--idle-exit")) idle_exit = (unsigned)arg_num(v);
        else {
            fprintf(stderr, "fl_peer: unknown option %s\n", a);
            return 2;
        }
        i++;
    }
    if ((listen_port == NULL) == (connect_to == NULL) || window == 0 || window > MAX_WINDOW ||
        slots == 0 || slots > MAX_SLOTS || retries == 0 || retries > 255 || rto == 0) {
        fprintf(stderr, "usage: fl_peer (--listen PORT | --connect HOST:PORT) [--window 1..%u] "
                        "[--slots 1..%u] [--rto MS] [--retries 1..255] [--seed N] "
                        "[--consume-every MS] [--idle-exit MS]\n", MAX_WINDOW, MAX_SLOTS);
        return 2;
    }

#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return 1;
        }
    }
#endif

    memset(&p, 0, sizeof p);
    p.sock = open_socket(listen_port, connect_to);
    if (p.sock == SOCK_INVALID) {
        fprintf(stderr, "fl_peer: could not open the socket\n");
        return 1;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.tx_buf = tx_buf;
    cfg.tx_buf_len = FL_TX_BUF_SIZE(window);
    cfg.rx_buf = rx_buf;
    cfg.rx_buf_len = FL_RX_BUF_SIZE(slots);
    cfg.window = (uint8_t)window;
    cfg.max_retries = (uint8_t)retries;
    cfg.rto_ms = (uint16_t)rto;
    cfg.session_seed = (uint8_t)(have_seed ? seed : now_ms());
    cfg.write_bytes = on_write;
    cfg.read_bytes = on_read;
    cfg.on_event = on_event;
    cfg.user = &p;
    if (fl_init(&link, &cfg) != FL_OK) {
        fprintf(stderr, "fl_peer: invalid configuration\n");
        return 1;
    }

    last_consume = p.last_rx = now_ms();
    while (!p.closed) {
        uint32_t t;
        wait_readable(p.sock, 1);
        t = now_ms();
        fl_poll(&link, t);

        /* Echo: take frames off the queue and send them back. A frame we
         * could not send yet is held until the window opens again. */
        for (;;) {
            if (!holding) {
                if (consume_every != 0 && (uint32_t)(t - last_consume) < consume_every) {
                    break;
                }
                if (fl_recv(&link, &held_type, held, sizeof held, &held_len) != FL_OK) {
                    break;
                }
                holding = 1;
                last_consume = t;
            }
            if (fl_send(&link, held_type, held, held_len, NULL) != FL_OK) {
                break;
            }
            holding = 0;
            echoed++;
            if (consume_every != 0) {
                break;
            }
        }

        /* The other side has gone quiet: we are done. Reads inside fl_poll
         * can stamp last_rx later than `t`, so compare as a signed
         * difference, as framelink's own timers do. */
        if (idle_exit != 0 && (int32_t)(now_ms() - p.last_rx) > (int32_t)idle_exit) {
            fprintf(stderr, "fl_peer: no data for %u ms, exiting\n", idle_exit);
            break;
        }
    }

    if (p.closed) {
        fprintf(stderr, "fl_peer: connection closed\n");
    }
    fl_get_stats(&link, &st);
    printf("{\"echoed\": %lu, \"rx_frames\": %lu, \"tx_frames\": %lu, \"hunt_bytes\": %lu, "
           "\"hdr_errors\": %lu, \"crc_errors\": %lu, \"resyncs\": %lu, \"timeouts\": %lu, "
           "\"retransmits\": %lu, \"dup_drops\": %lu, \"overflows\": %lu, \"unknown_type\": %lu, "
           "\"link_down\": %lu, \"peer_resets\": %u}\n",
           echoed, (unsigned long)st.rx_frames, (unsigned long)st.tx_frames,
           (unsigned long)st.hunt_bytes, (unsigned long)st.hdr_errors, (unsigned long)st.crc_errors,
           (unsigned long)st.resyncs, (unsigned long)st.timeouts, (unsigned long)st.retransmits,
           (unsigned long)st.dup_drops, (unsigned long)st.overflows, (unsigned long)st.unknown_type,
           (unsigned long)st.link_down, p.events[FL_EV_PEER_RESET]);
    fflush(stdout);
    sock_close(p.sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
