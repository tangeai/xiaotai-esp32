set(shared "${CMAKE_CURRENT_LIST_DIR}/../../../lckfb-szpi-esp32s3-tirtc/components/starter_console")
idf_component_register(
 SRCS "${shared}/src/starter_console.c"
 INCLUDE_DIRS "${shared}/include"
 REQUIRES console
 PRIV_REQUIRES esp_app_format esp_system esp_timer freertos platform_client runtime_config starter_media starter_product starter_runtime starter_tirtc starter_voice wifi_manager
)
