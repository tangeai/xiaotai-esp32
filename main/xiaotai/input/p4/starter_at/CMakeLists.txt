idf_component_register(
 SRCS "src/starter_at.c"
 INCLUDE_DIRS "include"
 PRIV_REQUIRES esp_driver_uart esp_timer esp_system freertos wifi_manager starter_product
)
