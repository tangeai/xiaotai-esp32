idf_component_register(
    SRCS "src/platform_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_netif mqtt mbedtls json
    PRIV_REQUIRES esp-tls esp_timer
)
