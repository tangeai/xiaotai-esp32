idf_component_register(SRCS "wt99p461_s1.c"
 INCLUDE_DIRS "include"
 REQUIRES esp_driver_i2c esp_driver_ledc esp_lcd esp_lcd_jd9165
          esp_lcd_touch_gt911 esp_lvgl_port lvgl
 PRIV_REQUIRES esp_hw_support esp_timer)
