# IDF scans requirements in a separate CMake process, before sdkconfig exists.
idf_build_get_property(XIAOTAI_BOARD_DIR XIAOTAI_BOARD_DIR)
idf_build_get_property(XIAOTAI_NATIVE_PROFILE XIAOTAI_NATIVE_PROFILE)
get_filename_component(board_root "${CMAKE_CURRENT_LIST_DIR}/boards" REALPATH)
get_filename_component(board_dir "${board_root}/${XIAOTAI_BOARD_DIR}" REALPATH)
string(FIND "${board_dir}/" "${board_root}/" board_prefix)
if(NOT board_prefix EQUAL 0 OR NOT EXISTS "${board_dir}/board.cmake")
    message(FATAL_ERROR "Board implementation is pending or outside the board tree: ${XIAOTAI_BOARD_DIR}")
endif()
include("${board_dir}/board.cmake")

if(XIAOTAI_NATIVE_PROFILE)
    idf_component_register(SRCS main.cc xiaotai/application/application.cc
        "${board_dir}/board.cc" INCLUDE_DIRS . boards/common xiaotai/application
        REQUIRES startup esp_common esp_system log)
    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_NATIVE_PROFILE=1)
        target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
    endif()
    return()
endif()

set(modules
    xiaotai/platform/nvs_worker xiaotai/services/device_platform
    xiaotai/platform/runtime_config xiaotai/platform/wifi_manager
    xiaotai/application/session xiaotai/input/button xiaotai/input/console
    xiaotai/input/at xiaotai/diagnostics
    xiaotai/transport/tirtc ${XIAOTAI_MEDIA_MODULE} xiaotai/media/voice ${XIAOTAI_UI_MODULE})
set(sources main.cc xiaotai/application/application.cc xiaotai/application/startup_services.c
    boards/common/board_bridge.cc ${XIAOTAI_BOARD_SOURCES})
set(includes . boards/common xiaotai/application)
foreach(module IN LISTS modules)
    file(GLOB module_sources "${module}/src/*.c" "${module}/src/*.cpp")
    list(APPEND sources ${module_sources})
    list(APPEND includes "${module}/include" "${module}/src")
endforeach()
list(APPEND sources xiaotai/media/voice/vendor/mel_extractor.c)
list(APPEND includes xiaotai/media/voice/vendor xiaotai/media/voice/model)
idf_component_register(SRCS ${sources} INCLUDE_DIRS ${includes}
    REQUIRES xiaotai_dependencies tirtc_sdk nvs_flash esp_wifi esp_netif esp_event esp_timer esp_system
        esp_http_client esp_http_server mqtt mbedtls cjson esp-tls tcp_transport
        esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_i2s esp_lcd
        esp_codec_dev esp_audio_codec esp-sr esp32-camera esp_new_jpeg
        esp_lvgl_port esp_lcd_touch_ft5x06 lvgl esp_app_format console
        esp-tflite-micro esp-dsp)
if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()
if(NOT CONFIG_BOARD_TYPE_LICHUANG_DEV_S3 OR NOT XIAOTAI_BOARD_DIR STREQUAL "lckfb/szpi-esp32s3")
    message(FATAL_ERROR "This board's business/media migration is pending; no fallback firmware is built")
endif()
foreach(feature AI DEVICE_CALL WECHAT_VOIP ROOM H5_STREAM H5_VIDEO AI_VISION)
    if(NOT CONFIG_XIAOTAI_${feature})
        message(FATAL_ERROR "Source/UI/capability trimming for ${feature} is not validated yet")
    endif()
endforeach()
idf_component_get_property(cjson_dir espressif__cjson COMPONENT_DIR)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/room_json.cmake")
starter_room_json_headers("${cjson_dir}/cJSON" "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=lwip_getaddrinfo" "-Wl,--wrap=esp_transport_connect"
    "-Wl,--wrap=esp_transport_write" "-Wl,--wrap=esp_transport_read")
target_add_binary_data(${COMPONENT_LIB} "xiaotai/platform/wifi_manager/web/setup.html" TEXT)
foreach(asset kids_watch_outgoing_tiny kids_watch_incoming_tiny)
    target_add_binary_data(${COMPONENT_LIB} "xiaotai/ui/lcd320/assets/rings/${asset}.wav" BINARY)
endforeach()
foreach(asset ai_ack_female_8k ai_ack_male_8k)
    target_add_binary_data(${COMPONENT_LIB} "xiaotai/ui/lcd320/assets/prompts/${asset}.wav" BINARY)
endforeach()
target_add_binary_data(${COMPONENT_LIB} "xiaotai/media/voice/model/nihaoxiaotai.tflite" BINARY)
set_source_files_properties(xiaotai/media/voice/src/starter_voice.c
    xiaotai/media/voice/src/wake_backend.cpp xiaotai/media/voice/vendor/mel_extractor.c
    xiaotai/media/s3/src/starter_aec.c xiaotai/media/s3/src/starter_audio_resampler.c
    xiaotai/ui/lcd320/src/starter_product.c PROPERTIES COMPILE_OPTIONS "-O2")
target_compile_features(${COMPONENT_LIB} PRIVATE c_std_17 cxx_std_23)
