idf_component_register(
 SRCS "src/starter_at.c" "src/coprocessor_update.c" "src/regression_controls.c"
 INCLUDE_DIRS "include"
 PRIV_REQUIRES esp_driver_uart esp_timer esp_system freertos wifi_manager starter_product p4_hardware
               starter_runtime starter_media starter_tirtc starter_voice esp_timer platform_client
               espressif__esp_hosted esp_http_client mbedtls bootloader_support esp_app_format
)
