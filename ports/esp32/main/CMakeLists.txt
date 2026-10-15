idf_component_register(
    SRCS "main.c" "common.c" "app_echo_uart.c" "app_loopback.c" "app_echo_tcp.c" "app_bench.c"
    INCLUDE_DIRS "."
    REQUIRES framelink esp_driver_uart esp_timer esp_wifi esp_netif esp_event nvs_flash lwip esp_rom)
