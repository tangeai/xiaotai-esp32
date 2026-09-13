idf_component_register(
    SRCS "src/starter_button.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio freertos log starter_runtime
)
