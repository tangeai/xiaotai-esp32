idf_component_register(SRCS "status_led.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_driver_rmt esp_timer)
