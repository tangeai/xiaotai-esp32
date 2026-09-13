idf_component_register(
    SRCS "src/starter_console.c"
    INCLUDE_DIRS "include"
    REQUIRES console
    PRIV_REQUIRES esp_app_format esp_system esp_timer freertos platform_client runtime_config
                  starter_media starter_product starter_runtime starter_tirtc starter_voice
                  wifi_manager
)
