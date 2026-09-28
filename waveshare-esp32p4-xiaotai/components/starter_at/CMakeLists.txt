idf_component_register(
 SRCS "src/starter_at.c"
 INCLUDE_DIRS "include"
 PRIV_REQUIRES driver esp_system freertos wifi_manager starter_product
)
