idf_component_register(SRCS "src/nvs_store.c" INCLUDE_DIRS "include"
 REQUIRES esp_common PRIV_REQUIRES nvs_flash freertos esp_timer)
