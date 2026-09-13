idf_component_register(
    SRCS "src/starter_runtime.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES esp_timer esp_system freertos platform_client runtime_config
                  starter_media starter_tirtc wifi_manager
)
