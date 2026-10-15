/*
 * Echo peer over Wi-Fi TCP. Set the network in menuconfig under
 * "framelink demo > Wi-Fi", flash, and read the IP address from the console:
 *
 *   framelink bench tcp:192.168.1.50:5000 --count 1000
 *
 * One client at a time. Each connection gets a fresh framelink session.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Guilherme Schwonka
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "fl_tcp";
static EventGroupHandle_t wifi_events;
#define WIFI_CONNECTED BIT0

static uint8_t tx_buf[FL_TX_BUF_SIZE(CONFIG_FL_WINDOW)];
static uint8_t rx_buf[FL_RX_BUF_SIZE(CONFIG_FL_SLOTS)];
static fl_link peer_link;
static echo_state echo;

typedef struct conn {
    int sock;
    int closed;
} conn;

static int tcp_write(void *user, const uint8_t *data, size_t len)
{
    conn *c = (conn *)user;
    size_t done = 0;
    while (done < len && !c->closed) {
        int n = send(c->sock, data + done, len - done, 0);
        if (n <= 0) {
            c->closed = 1;
            break;
        }
        done += (size_t)n;
    }
    return (int)done;
}

static size_t tcp_read(void *user, uint8_t *data, size_t cap)
{
    conn *c = (conn *)user;
    int n = recv(c->sock, data, cap, MSG_DONTWAIT);
    if (n > 0) {
        return (size_t)n;
    }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        c->closed = 1;
    }
    return 0;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        printf("framelink TCP echo peer at " IPSTR ":%d\n", IP2STR(&ev->ip_info.ip), CONFIG_FL_TCP_PORT);
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED);
    }
}

static void wifi_start(void)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t sta = {0};
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    strncpy((char *)sta.sta.ssid, CONFIG_FL_WIFI_SSID, sizeof sta.sta.ssid);
    strncpy((char *)sta.sta.password, CONFIG_FL_WIFI_PASSWORD, sizeof sta.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    /* Power saving adds tens of milliseconds of latency to every frame. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void serve(int client)
{
    conn c = {.sock = client, .closed = 0};
    int one = 1;
    fl_config cfg = {
        .tx_buf = tx_buf,
        .tx_buf_len = sizeof tx_buf,
        .rx_buf = rx_buf,
        .rx_buf_len = sizeof rx_buf,
        .window = CONFIG_FL_WINDOW,
        .max_retries = 255,
        .rto_ms = CONFIG_FL_RTO_MS,
        .session_seed = (uint8_t)esp_random(),
        .write_bytes = tcp_write,
        .read_bytes = tcp_read,
        .user = &c,
    };

    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    ESP_ERROR_CHECK(fl_init(&peer_link, &cfg) == FL_OK ? ESP_OK : ESP_ERR_INVALID_ARG);
    memset(&echo, 0, sizeof echo);
    while (!c.closed) {
        fl_poll(&peer_link, fl_now_ms());
        fl_echo_step(&peer_link, &echo);
        vTaskDelay(1);
    }
    ESP_LOGI(TAG, "client left after %u echoed frames", (unsigned)echo.echoed);
    close(client);
}

void app_echo_tcp(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_FL_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int server;

    if (strlen(CONFIG_FL_WIFI_SSID) == 0) {
        printf("FLERROR set the Wi-Fi network in menuconfig: framelink demo > Wi-Fi\n");
        return;
    }
    wifi_start();
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED, pdFALSE, pdTRUE, portMAX_DELAY);

    server = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    ESP_ERROR_CHECK(server < 0 ? ESP_FAIL : ESP_OK);
    ESP_ERROR_CHECK(bind(server, (struct sockaddr *)&addr, sizeof addr) == 0 ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(listen(server, 1) == 0 ? ESP_OK : ESP_FAIL);
    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client >= 0) {
            ESP_LOGI(TAG, "client connected");
            serve(client);
        }
    }
}
