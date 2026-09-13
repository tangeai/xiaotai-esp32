set(shared "${CMAKE_CURRENT_LIST_DIR}/../../lckfb-szpi-esp32s3-tirtc/main")
idf_component_register(SRCS "${shared}/app_main.c" INCLUDE_DIRS "."
 PRIV_REQUIRES esp_app_format esp_system esp_wifi nvs_flash platform_client runtime_config
 starter_button starter_console starter_media starter_product starter_runtime starter_tirtc
 starter_voice wifi_manager p4_hardware)
