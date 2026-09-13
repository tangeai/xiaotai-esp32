set(shared "${CMAKE_CURRENT_LIST_DIR}/../../../lckfb-szpi-esp32s3-tirtc/components/wifi_manager")
idf_component_register(
 SRCS "${shared}/src/wifi_manager.c" "${shared}/src/captive_dns.c"
 INCLUDE_DIRS "${shared}/include"
 PRIV_INCLUDE_DIRS "${shared}/src"
 REQUIRES esp_wifi esp_netif esp_event nvs_flash
 PRIV_REQUIRES esp_http_server json esp_system esp_timer lwip
)
