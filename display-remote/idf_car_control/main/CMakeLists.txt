idf_component_register(
    SRCS "main.c" "car_ui.c" "car_net.c"
    INCLUDE_DIRS "."
    REQUIRES board lvgl espressif__esp_lvgl_port nvs_flash esp_wifi esp_netif
             esp_event esp_http_client espressif__mdns
)
