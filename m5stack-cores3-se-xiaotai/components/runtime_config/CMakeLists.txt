idf_component_register(
    SRCS "src/runtime_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash nvs_worker
)
