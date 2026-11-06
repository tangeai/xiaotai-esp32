idf_component_register(SRCS "starter_board.c" INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_i2s esp_timer freertos heap)
