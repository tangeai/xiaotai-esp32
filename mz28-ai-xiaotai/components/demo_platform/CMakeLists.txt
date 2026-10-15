idf_component_register(SRCS "platform.c" "platform_http.c" "platform_network.c" "message_buffer.c"
                       INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer
                                esp_http_client mqtt json mbedtls)
