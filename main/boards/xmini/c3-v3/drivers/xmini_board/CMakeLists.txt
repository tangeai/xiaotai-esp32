idf_component_register(SRCS "xmini_board.c" "bounded_lcd_i2c.c" INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c esp_driver_gpio esp_lcd)
