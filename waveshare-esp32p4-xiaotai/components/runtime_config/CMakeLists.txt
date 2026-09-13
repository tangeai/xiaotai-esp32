set(shared "${CMAKE_CURRENT_LIST_DIR}/../../../lckfb-szpi-esp32s3-tirtc/components/runtime_config")
idf_component_register(
 SRCS "${shared}/src/runtime_config.c"
 INCLUDE_DIRS "${shared}/include"
 REQUIRES nvs_flash
)
