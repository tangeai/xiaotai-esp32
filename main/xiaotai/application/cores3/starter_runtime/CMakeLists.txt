idf_component_register(
    SRCS "src/starter_runtime.c" "src/room_json.c"
    INCLUDE_DIRS "include"
    REQUIRES cjson
    PRIV_REQUIRES esp_app_format esp_timer esp_system freertos platform_client runtime_config
                  starter_media starter_tirtc wifi_manager
)

if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()
idf_component_get_property(room_cjson_dir espressif__cjson COMPONENT_DIR)
include("${CMAKE_CURRENT_LIST_DIR}/../../../../boards/m5stack/core-s3/build-support/cmake/room_json.cmake")
starter_room_json_headers("${room_cjson_dir}/cJSON" "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
