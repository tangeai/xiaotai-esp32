idf_component_register(
 SRCS "${CMAKE_CURRENT_LIST_DIR}/src/runtime_config.c"
 INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include"
 REQUIRES nvs_flash nvs_store
)
