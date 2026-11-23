# SPDX-License-Identifier: MIT
include("${CMAKE_CURRENT_LIST_DIR}/../../../sdk/cmake/xiaotai_board.cmake")
set(manifest "${CMAKE_CURRENT_LIST_DIR}/board.json")
set(image "${CMAKE_CURRENT_LIST_DIR}/assets/waveshare_c6_app.bin")
set(parts xiaotai_part_bus xiaotai_part_audio xiaotai_part_button
    xiaotai_part_display xiaotai_part_panel_spi xiaotai_part_panel_st7796
    xiaotai_part_touch_ft5x06 xiaotai_part_camera_csi xiaotai_part_coprocessor)
if(ESP_PLATFORM)
    idf_component_register(SRCS "${CMAKE_CURRENT_LIST_DIR}/board.cc"
                           REQUIRES xiaotai_hal PRIV_REQUIRES ${parts}
                           EMBED_FILES "${image}" WHOLE_ARCHIVE)
    if(CMAKE_BUILD_EARLY_EXPANSION)
        return()
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/build-support/check_board_config.cmake")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${image}")
file(SIZE "${image}" image_size)
file(SHA256 "${image}" image_sha)
if(NOT image_size EQUAL 1111904 OR
   NOT image_sha STREQUAL "8700a7215c77ffc9810ab27dbf7687d6222c620c13d7814dc502ace6a784b4f5")
    message(FATAL_ERROR "Waveshare P4 C6 APP does not match the existing approved image")
endif()
xiaotai_board_header("${manifest}" generated)
if(ESP_PLATFORM)
    set(board_lib ${COMPONENT_LIB})
else()
    enable_language(ASM)
    xiaotai_host_require(xiaotai_hal ${parts})
    set(assembly "${CMAKE_CURRENT_BINARY_DIR}/c6_app.S")
    configure_file(assets/c6_app_host.S.in "${assembly}" @ONLY)
    set_source_files_properties("${assembly}" PROPERTIES OBJECT_DEPENDS "${image}")
    add_library(xiaotai_board STATIC board.cc "${assembly}")
    set(board_lib xiaotai_board)
    target_link_libraries("${board_lib}" PUBLIC xiaotai_hal PRIVATE ${parts})
endif()
target_include_directories("${board_lib}" PRIVATE "${generated}")
target_compile_definitions("${board_lib}" PRIVATE
    "XIAOTAI_C6_IMAGE_SIZE=${image_size}" "XIAOTAI_C6_IMAGE_SHA=\"${image_sha}\"")
if(NOT ESP_PLATFORM AND XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
