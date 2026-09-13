set(shared "${CMAKE_CURRENT_LIST_DIR}/../../../lckfb-szpi-esp32s3-tirtc/components/platform_client")
idf_component_register(
 SRCS "${shared}/src/platform_client.c"
 INCLUDE_DIRS "${shared}/include"
 REQUIRES esp_http_client esp_netif mqtt mbedtls json
 PRIV_REQUIRES esp-tls esp_timer
)
