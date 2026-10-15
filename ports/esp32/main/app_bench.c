/*
 * Decoder and encoder cost in CPU cycles, measured on the ESP32 itself.
 *
 * The decoder is fed from RAM, with no UART involved, so only framelink's
 * own work is counted. Interrupts are disabled while each chunk is timed with
 * the CPU cycle counter. Four inputs, as in docs/MEASUREMENTS.md section 1:
 *
 *   clean_16     back-to-back valid frames with 16-byte payloads
 *   clean_255    back-to-back valid frames with 255-byte payloads
 *   garbage      random bytes without a single SYNC byte
 *   adversarial  back-to-back valid headers that each announce 255 payload
 *                bytes. Every candidate fails its CRC16 only after buffering
 *                a full frame, and the rescan finds the next header 5 bytes
 *                later. This is the worst case of the rescanning decoder.
 *
 * Build once with sdkconfig.bench and once with sdkconfig.bench_table to
 * compare the bitwise and table-driven CRC16.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "esp_cpu.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifdef FL_CRC_TABLE
#define FL_CRC_NAME "table"
#else
#define FL_CRC_NAME "bitwise"
#endif

#define INPUT_SIZE (32 * 1024)
#define CHUNK 1024

static fl_decoder dec;
static uint32_t frames_seen;

static void count_frame(void *user, const fl_frame *f)
{
    (void)user;
    (void)f;
    frames_seen++;
}

static size_t fill_frames(uint8_t *buf, size_t cap, size_t payload)
{
    uint8_t p[FL_MAX_PAYLOAD];
    size_t n = 0, len;
    uint8_t seq = 0;
    while (n + FL_FRAME_SIZE(payload) <= cap) {
        size_t k;
        for (k = 0; k < payload; k++) {
            p[k] = (uint8_t)esp_random();
        }
        fl_encode(0x01, seq++, p, payload, buf + n, cap - n, &len);
        n += len;
    }
    return n;
}

static size_t fill_garbage(uint8_t *buf, size_t cap)
{
    size_t i;
    for (i = 0; i < cap; i++) {
        uint8_t b = (uint8_t)esp_random();
        buf[i] = b == FL_SYNC ? 0x00 : b;
    }
    return cap;
}

static size_t fill_adversarial(uint8_t *buf, size_t cap)
{
    size_t n = 0;
    while (n + FL_HEADER_LEN <= cap) {
        buf[n] = FL_SYNC;
        buf[n + 1] = 255;
        buf[n + 2] = 0x01;
        buf[n + 3] = (uint8_t)n;
        buf[n + 4] = fl_crc8(&buf[n + 1], 3);
        n += FL_HEADER_LEN;
    }
    return n;
}

/* Cycles to decode `len` bytes, timed in chunks with interrupts off. */
static uint64_t time_decode(const uint8_t *buf, size_t len)
{
    uint64_t total = 0;
    size_t off;
    fl_decoder_init(&dec);
    frames_seen = 0;
    for (off = 0; off < len; off += CHUNK) {
        size_t n = len - off < CHUNK ? len - off : CHUNK;
        uint32_t start, end;
        portDISABLE_INTERRUPTS();
        start = esp_cpu_get_cycle_count();
        fl_decoder_feed(&dec, buf + off, n, count_frame, NULL);
        end = esp_cpu_get_cycle_count();
        portENABLE_INTERRUPTS();
        total += (uint32_t)(end - start);
        vTaskDelay(1); /* keep the watchdogs fed */
    }
    return total;
}

static void report_decode(const char *name, const uint8_t *buf, size_t len)
{
    uint64_t cycles = time_decode(buf, len);
    printf(FL_RESULT_PREFIX "{\"test\": \"decode\", \"input\": \"%s\", \"crc\": \"%s\", "
           "\"cpu_mhz\": %d, \"bytes\": %u, \"frames\": %" PRIu32 ", \"cycles_per_byte\": %.1f, "
           "\"ns_per_byte\": %.1f}\n",
           name, FL_CRC_NAME, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, (unsigned)len, frames_seen,
           (double)cycles / len, (double)cycles / len * 1000.0 / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
}

static void report_encode(size_t payload)
{
    uint8_t p[FL_MAX_PAYLOAD] = {0}, out[FL_MAX_FRAME];
    const int rounds = 2000;
    uint64_t total = 0;
    int i;
    for (i = 0; i < rounds; i++) {
        uint32_t start, end;
        portDISABLE_INTERRUPTS();
        start = esp_cpu_get_cycle_count();
        fl_encode(0x01, (uint8_t)i, p, payload, out, sizeof out, NULL);
        end = esp_cpu_get_cycle_count();
        portENABLE_INTERRUPTS();
        total += (uint32_t)(end - start);
        if (i % 200 == 0) {
            vTaskDelay(1);
        }
    }
    printf(FL_RESULT_PREFIX "{\"test\": \"encode\", \"payload\": %u, \"crc\": \"%s\", "
           "\"cycles_per_frame\": %.0f, \"cycles_per_byte\": %.1f}\n",
           (unsigned)payload, FL_CRC_NAME, (double)total / rounds,
           (double)total / rounds / FL_FRAME_SIZE(payload));
}

void app_bench(void)
{
    uint8_t *buf = malloc(INPUT_SIZE); /* the app may allocate; the library never does */
    size_t n;

    vTaskDelay(pdMS_TO_TICKS(500));
    printf("framelink decoder benchmark, %s CRC16, %d MHz\n", FL_CRC_NAME, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    printf(FL_RESULT_PREFIX "{\"test\": \"memory\", \"sizeof_fl_decoder\": %u, \"sizeof_fl_link\": %u, "
           "\"tx_buf_window_4\": %u, \"rx_buf_slots_4\": %u}\n",
           (unsigned)sizeof(fl_decoder), (unsigned)sizeof(fl_link),
           (unsigned)FL_TX_BUF_SIZE(4), (unsigned)FL_RX_BUF_SIZE(4));

    n = fill_frames(buf, INPUT_SIZE, 16);
    report_decode("clean_16", buf, n);
    n = fill_frames(buf, INPUT_SIZE, 255);
    report_decode("clean_255", buf, n);
    n = fill_garbage(buf, INPUT_SIZE);
    report_decode("garbage", buf, n);
    n = fill_adversarial(buf, INPUT_SIZE);
    report_decode("adversarial", buf, n);
    report_encode(16);
    report_encode(255);

    free(buf);
    printf("FLDONE\n");
}
