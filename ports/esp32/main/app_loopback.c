/*
 * UART1 to UART2 loopback on one ESP32: two framelink endpoints talking over
 * a real UART, connected with two wires:
 *
 *   GPIO25 (UART1 TX)  ->  GPIO16 (UART2 RX)
 *   GPIO17 (UART2 TX)  ->  GPIO26 (UART1 RX)
 *
 * Endpoint A sends as fast as framelink lets it. Endpoint B is the receiver
 * and consumes frames at a fixed rate. Each test runs for
 * CONFIG_FL_LOOP_SECONDS and prints one FLRESULT line:
 *
 *   - at 115200 and 921600 baud,
 *   - with a consumer at 50% of the line rate, with credits honoured, then
 *     with A ignoring credits (the failure flow control exists to prevent),
 *   - and with an unthrottled consumer, to measure raw link efficiency.
 *
 * Every payload carries a counter and a CRC-32 checked outside framelink, so
 * loss, duplication, reordering and undetected corruption would all show.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#include <inttypes.h>
#include <stdio.h>

#include "common.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define PAYLOAD 64
#define WINDOW 16
#define SLOTS 4

typedef struct endpoint {
    fl_link link;
    uint8_t tx[FL_TX_BUF_SIZE(WINDOW)];
    uint8_t rx[FL_RX_BUF_SIZE(SLOTS)];
} endpoint;

static endpoint A, B;

static int start(endpoint *e, uart_port_t port, int baud, int tx, int rx)
{
    fl_config cfg = {
        .tx_buf = e->tx,
        .tx_buf_len = sizeof e->tx,
        .rx_buf = e->rx,
        .rx_buf_len = sizeof e->rx,
        .window = WINDOW,
        .max_retries = 50,
        .rto_ms = baud >= 460800 ? 20 : 60,
        .session_seed = (uint8_t)esp_random(),
        .write_bytes = fl_uart_write,
        .read_bytes = fl_uart_read,
        .user = (void *)(intptr_t)port,
    };
    fl_uart_setup(port, baud, tx, rx);
    return fl_init(&e->link, &cfg) == FL_OK;
}

static void run_test(int baud, int flow_control, int throttled)
{
    /* UART 8N1 carries 10 bits per byte. Each frame adds 7 bytes. */
    const double ceiling = (baud / 10.0) * PAYLOAD / (PAYLOAD + FL_OVERHEAD);
    const int64_t interval_us = throttled ? (int64_t)(1e6 * PAYLOAD / (ceiling * 0.5)) : 0;
    uint8_t buf[PAYLOAD];
    uint32_t sent = 0, expected = 0, bad = 0, out_of_order = 0;
    int64_t t0, t_end, next_consume;
    fl_stats sa, sb;
    int i;

    start(&A, UART_NUM_1, baud, CONFIG_FL_LOOP_TX1, CONFIG_FL_LOOP_RX1);
    start(&B, UART_NUM_2, baud, CONFIG_FL_LOOP_TX2, CONFIG_FL_LOOP_RX2);

    for (i = 0; i < 2000 && !(fl_is_connected(&A.link) && fl_is_connected(&B.link)); i++) {
        fl_poll(&A.link, fl_now_ms());
        fl_poll(&B.link, fl_now_ms());
        vTaskDelay(1);
    }
    if (!fl_is_connected(&A.link) || !fl_is_connected(&B.link)) {
        printf("FLERROR no handshake at %d baud. Check the wires: GPIO%d -> GPIO%d and GPIO%d -> GPIO%d\n",
               baud, CONFIG_FL_LOOP_TX1, CONFIG_FL_LOOP_RX2, CONFIG_FL_LOOP_TX2, CONFIG_FL_LOOP_RX1);
        return;
    }

    t0 = esp_timer_get_time();
    t_end = t0 + (int64_t)CONFIG_FL_LOOP_SECONDS * 1000000;
    next_consume = t0;
    while (esp_timer_get_time() < t_end) {
        uint32_t now = fl_now_ms();
        int64_t now_us = esp_timer_get_time();
        uint8_t type;
        size_t len;

        fl_poll(&A.link, now);
        fl_poll(&B.link, now);

        for (;;) {
            if (!flow_control) {
                A.link.peer_credits = 255; /* a sender that ignores credits */
            }
            fl_make_payload(sent, buf, sizeof buf);
            if (fl_send(&A.link, 0x01, buf, sizeof buf, NULL) != FL_OK) {
                break;
            }
            sent++;
        }

        while (!throttled || now_us >= next_consume) {
            uint32_t index;
            if (fl_recv(&B.link, &type, buf, sizeof buf, &len) != FL_OK) {
                break;
            }
            if (!fl_check_payload(buf, len, &index)) {
                bad++;
            } else if (index != expected) {
                out_of_order++;
                expected = index + 1;
            } else {
                expected++;
            }
            next_consume += interval_us;
        }
        if (throttled && next_consume < now_us - 100000) {
            next_consume = now_us; /* do not bank credit after a stall */
        }
        vTaskDelay(1);
    }

    {
        double seconds = (esp_timer_get_time() - t0) / 1e6;
        double goodput = expected * (double)PAYLOAD / seconds;
        fl_get_stats(&A.link, &sa);
        fl_get_stats(&B.link, &sb);
        printf(FL_RESULT_PREFIX "{\"test\": \"throughput\", \"baud\": %d, \"payload\": %d, "
               "\"window\": %d, \"slots\": %d, \"consumer\": \"%s\", \"flow_control\": %s, "
               "\"seconds\": %.2f, \"delivered\": %" PRIu32 ", \"goodput_Bps\": %.0f, "
               "\"ceiling_Bps\": %.0f, \"percent_of_ceiling\": %.1f, \"overflows\": %" PRIu32 ", "
               "\"retransmits\": %" PRIu32 ", \"crc_errors\": %" PRIu32 ", \"undetected\": %" PRIu32 ", "
               "\"out_of_order\": %" PRIu32 ", \"link_down\": %" PRIu32 "}\n",
               baud, PAYLOAD, WINDOW, SLOTS, throttled ? "50%" : "unthrottled",
               flow_control ? "true" : "false", seconds, expected, goodput, ceiling,
               100.0 * goodput / ceiling, sb.overflows, sa.retransmits, sb.crc_errors + sa.crc_errors,
               bad, out_of_order, sa.link_down + sb.link_down);
    }
}

void app_loopback(void)
{
    static const int bauds[] = {115200, 921600};
    unsigned i;

    vTaskDelay(pdMS_TO_TICKS(500));
    printf("framelink loopback: UART1 GPIO%d/%d <-> UART2 GPIO%d/%d, %d s per test\n",
           CONFIG_FL_LOOP_TX1, CONFIG_FL_LOOP_RX1, CONFIG_FL_LOOP_TX2, CONFIG_FL_LOOP_RX2,
           CONFIG_FL_LOOP_SECONDS);
    for (i = 0; i < sizeof bauds / sizeof bauds[0]; i++) {
        run_test(bauds[i], 1, 1);
        run_test(bauds[i], 0, 1);
        run_test(bauds[i], 1, 0);
    }
    printf("FLDONE\n");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
