idf_component_register(SRCS "esp_lcd_jd9165.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd
                       PRIV_REQUIRES esp_driver_gpio freertos)
