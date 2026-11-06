idf_component_register(
    SRCS "src/starter_button.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES starter_board freertos log starter_runtime
)
