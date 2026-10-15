/*
 * Echo peer on UART0, the board's USB serial port.
 *
 * Every frame the host sends comes straight back. The Python host uses this
 * for interop tests, throughput and recovery measurements:
 *
 *   framelink bench serial:COM5@921600 --count 1000
 *   python bench/host_recovery.py --device serial:COM5@921600
 *
 * The console and all logging are disabled in this app (sdkconfig.echo_uart),
 * so the port carries framelink only. The ROM bootloader still prints a few
 * lines at reset; the decoder simply discards them as noise.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#include "common.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static uint8_t tx_buf[FL_TX_BUF_SIZE(CONFIG_FL_WINDOW)];
static uint8_t rx_buf[FL_RX_BUF_SIZE(CONFIG_FL_SLOTS)];
static fl_link link;
static echo_state echo;

void app_echo_uart(void)
{
    fl_config cfg = {
        .tx_buf = tx_buf,
        .tx_buf_len = sizeof tx_buf,
        .rx_buf = rx_buf,
        .rx_buf_len = sizeof rx_buf,
        .window = CONFIG_FL_WINDOW,
        .max_retries = 255,
        .rto_ms = CONFIG_FL_RTO_MS,
        .session_seed = (uint8_t)esp_random(),
        .write_bytes = fl_uart_write,
        .read_bytes = fl_uart_read,
        .user = (void *)(intptr_t)UART_NUM_0,
    };

    fl_uart_setup(UART_NUM_0, CONFIG_FL_UART_BAUD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_ERROR_CHECK(fl_init(&link, &cfg) == FL_OK ? ESP_OK : ESP_ERR_INVALID_ARG);

    for (;;) {
        fl_poll(&link, fl_now_ms());
        fl_echo_step(&link, &echo);
        vTaskDelay(1); /* 1 ms with CONFIG_FREERTOS_HZ=1000 */
    }
}
