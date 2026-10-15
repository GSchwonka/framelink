/*
 * framelink on ESP32: entry point. The application is chosen at build time,
 * see Kconfig.projbuild and README.md.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#include "sdkconfig.h"

void app_echo_uart(void);
void app_loopback(void);
void app_echo_tcp(void);
void app_bench(void);

void app_main(void)
{
#if CONFIG_FL_APP_ECHO_UART
    app_echo_uart();
#elif CONFIG_FL_APP_LOOPBACK
    app_loopback();
#elif CONFIG_FL_APP_ECHO_TCP
    app_echo_tcp();
#elif CONFIG_FL_APP_BENCH
    app_bench();
#endif
}
