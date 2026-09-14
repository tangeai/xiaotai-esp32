idf_component_register(SRCS "xiaotai_main.c" INCLUDE_DIRS "."
 PRIV_REQUIRES esp_app_format esp_system esp_wifi nvs_flash nvs_store platform_client runtime_config
 starter_button starter_console starter_media starter_product starter_runtime starter_tirtc
 starter_voice wifi_manager p4_hardware)
