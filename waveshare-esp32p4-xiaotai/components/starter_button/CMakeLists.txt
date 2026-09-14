idf_component_register(SRCS "${CMAKE_CURRENT_LIST_DIR}/src/starter_button.c" INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include"
 PRIV_REQUIRES esp_driver_gpio freertos log starter_runtime)
