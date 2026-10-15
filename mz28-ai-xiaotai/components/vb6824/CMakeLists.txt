idf_component_register(SRCS "vb6824.c" "vb6824_wire.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_uart freertos)
