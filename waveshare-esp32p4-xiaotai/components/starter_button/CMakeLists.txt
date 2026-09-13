set(shared "${CMAKE_CURRENT_LIST_DIR}/../../../lckfb-szpi-esp32s3-tirtc/components/starter_button")
idf_component_register(SRCS "${shared}/src/starter_button.c" INCLUDE_DIRS "${shared}/include"
 PRIV_REQUIRES esp_driver_gpio freertos log starter_runtime)
