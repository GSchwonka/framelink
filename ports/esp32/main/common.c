/*
 * Helpers shared by the framelink ESP32 demo apps.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#include "common.h"

#include <string.h>

#include "esp_rom_crc.h"
#include "esp_timer.h"

uint32_t fl_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void fl_uart_setup(uart_port_t port, int baud, int tx_pin, int rx_pin)
{
    const uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_is_driver_installed(port)) {
        uart_driver_delete(port);
    }
    /* 4 KB each way: several maximum-size frames, and more than 40 ms of
     * data at 921600 baud, so a busy task never loses bytes. */
    ESP_ERROR_CHECK(uart_driver_install(port, 4096, 4096, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(port, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

int fl_uart_write(void *user, const uint8_t *data, size_t len)
{
    return uart_write_bytes((uart_port_t)(intptr_t)user, data, len);
}

size_t fl_uart_read(void *user, uint8_t *data, size_t cap)
{
    int n = uart_read_bytes((uart_port_t)(intptr_t)user, data, (uint32_t)cap, 0);
    return n > 0 ? (size_t)n : 0;
}

void fl_echo_step(fl_link *link, echo_state *e)
{
    for (;;) {
        if (!e->holding) {
            if (fl_recv(link, &e->type, e->buf, sizeof e->buf, &e->len) != FL_OK) {
                return;
            }
            e->holding = 1;
        }
        if (fl_send(link, e->type, e->buf, e->len, NULL) != FL_OK) {
            return;
        }
        e->holding = 0;
        e->echoed++;
    }
}

size_t fl_make_payload(uint32_t index, uint8_t *out, size_t size)
{
    size_t k;
    uint32_t crc;
    if (size < 8) {
        size = 8;
    }
    memcpy(out, &index, 4); /* the ESP32 is little-endian, like the host */
    for (k = 4; k < size - 4; k++) {
        out[k] = (uint8_t)(index * 131u + (k - 4));
    }
    crc = esp_rom_crc32_le(0, out, size - 4);
    memcpy(out + size - 4, &crc, 4);
    return size;
}

int fl_check_payload(const uint8_t *p, size_t len, uint32_t *index)
{
    uint32_t crc;
    if (len < 8) {
        return 0;
    }
    memcpy(&crc, p + len - 4, 4);
    if (esp_rom_crc32_le(0, p, len - 4) != crc) {
        return 0;
    }
    memcpy(index, p, 4);
    return 1;
}
