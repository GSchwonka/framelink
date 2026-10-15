/*
 * Helpers shared by the framelink ESP32 demo apps.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "framelink.h"

/* Millisecond clock for fl_poll. Wraps after 49.7 days, which framelink
 * handles. */
uint32_t fl_now_ms(void);

/* Install the UART driver on `port`. Pass UART_PIN_NO_CHANGE to keep the
 * default pins, which on UART0 are the USB serial bridge. */
void fl_uart_setup(uart_port_t port, int baud, int tx_pin, int rx_pin);

/* fl_config callbacks for a UART. Set cfg.user to (void *)(intptr_t)port. */
int fl_uart_write(void *user, const uint8_t *data, size_t len);
size_t fl_uart_read(void *user, uint8_t *data, size_t cap);

/* Echo: take received frames off the queue and send them back. A frame that
 * cannot be sent yet is held until the window opens. */
typedef struct echo_state {
    int holding;
    uint8_t type;
    uint8_t buf[FL_MAX_PAYLOAD];
    size_t len;
    uint32_t echoed;
} echo_state;

void fl_echo_step(fl_link *link, echo_state *e);

/* Test payloads: a 32-bit frame counter, filler, and a CRC-32 of the rest,
 * checked outside framelink. Same format as host/src/framelink/bench.py. */
size_t fl_make_payload(uint32_t index, uint8_t *out, size_t size);

/* Returns 1 and the index if the payload is intact, 0 otherwise. */
int fl_check_payload(const uint8_t *p, size_t len, uint32_t *index);

/* Results are printed as one line "FLRESULT {json}" so that
 * bench/esp32_collect.py can pick them out of the console output. */
#define FL_RESULT_PREFIX "FLRESULT "
