idf_component_register(
 SRCS "${CMAKE_CURRENT_LIST_DIR}/src/starter_console.c"
 INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include"
 REQUIRES console
 PRIV_REQUIRES esp_app_format esp_system esp_timer freertos platform_client runtime_config starter_media starter_product starter_runtime starter_tirtc starter_voice wifi_manager
)
