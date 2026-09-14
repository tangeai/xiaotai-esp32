idf_component_register(
 SRCS "${CMAKE_CURRENT_LIST_DIR}/src/starter_product.c" "${CMAKE_CURRENT_LIST_DIR}/src/ui_font_cn_18.c"
      "${CMAKE_CURRENT_LIST_DIR}/src/ui_font_cn_18_supplement.c"
 INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include"
 PRIV_INCLUDE_DIRS "src"
 REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd nvs_flash nvs_store platform_client
          starter_media starter_runtime starter_voice wifi_manager esp_lvgl_port
          esp_lcd_touch_ft5x06 lvgl esp_app_format p4_hardware)
# Keep the original symbol names, after making asset paths project-local.
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/assets/rings/kids_watch_outgoing_tiny.wav" BINARY RENAME_TO "kids_watch_outgoing_tiny.wav")
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/assets/rings/kids_watch_incoming_tiny.wav" BINARY RENAME_TO "kids_watch_incoming_tiny.wav")
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/assets/prompts/ai_ack_female_8k.wav" BINARY RENAME_TO "ai_ack_female_8k.wav")
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/assets/prompts/ai_ack_male_8k.wav" BINARY RENAME_TO "ai_ack_male_8k.wav")
