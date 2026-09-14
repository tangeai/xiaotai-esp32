idf_component_register(
 SRCS "${CMAKE_CURRENT_LIST_DIR}/src/starter_tirtc.c"
 INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include"
 REQUIRES tirtc_sdk freertos
 PRIV_REQUIRES log
)
