idf_component_register(
    SRCS "src/starter_tirtc.c"
    INCLUDE_DIRS "include"
    REQUIRES tirtc_sdk freertos
    PRIV_REQUIRES log
)
