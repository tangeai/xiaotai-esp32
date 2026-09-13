set(shared "${CMAKE_CURRENT_LIST_DIR}/../../../lckfb-szpi-esp32s3-tirtc/components/starter_tirtc")
idf_component_register(
 SRCS "${shared}/src/starter_tirtc.c"
 INCLUDE_DIRS "${shared}/include"
 REQUIRES tirtc_sdk freertos
 PRIV_REQUIRES log
)
