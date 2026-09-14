idf_component_register(
    SRCS "src/wifi_manager.c" "src/captive_dns.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash nvs_worker
    PRIV_REQUIRES esp_http_server json esp_system esp_timer lwip
)
