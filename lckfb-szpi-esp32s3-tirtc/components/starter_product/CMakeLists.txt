# S3 adds only missing legacy glyphs; P4 keeps its existing typography.
if(CONFIG_IDF_TARGET_ESP32S3)
    set(product_font "src/ui_font_cn_18.c" "src/ui_font_cn_18_supplement.c")
else()
    set(product_font "src/ui_font_cn_16.c")
endif()

idf_component_register(
    SRCS "src/starter_product.c" ${product_font}
    EMBED_FILES "assets/rings/kids_watch_outgoing_tiny.wav"
                "assets/rings/kids_watch_incoming_tiny.wav"
                "assets/prompts/ai_ack_female_8k.wav"
                "assets/prompts/ai_ack_male_8k.wav"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd nvs_flash platform_client starter_media starter_runtime starter_voice wifi_manager
             esp_lvgl_port esp_lcd_touch_ft5x06 lvgl esp_app_format
)

# Procedural RGB565 animation runs alongside wake inference. Optimize the
# renderer without changing its 25 fps cadence, geometry or task priority.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
