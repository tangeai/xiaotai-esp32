# SPDX-License-Identifier: MIT
include("${CMAKE_CURRENT_LIST_DIR}/../../../sdk/cmake/xiaotai_board.cmake")
set(manifest "${CMAKE_CURRENT_LIST_DIR}/board.json")
set(parts xiaotai_part_bus xiaotai_part_audio xiaotai_part_display
    xiaotai_part_panel_jd9165 xiaotai_part_touch_gt911 xiaotai_part_button
    xiaotai_part_camera_csi xiaotai_part_coprocessor)
if(ESP_PLATFORM)
    idf_component_register(SRCS "${CMAKE_CURRENT_LIST_DIR}/board.cc"
                           REQUIRES xiaotai_hal PRIV_REQUIRES ${parts} WHOLE_ARCHIVE)
    if(CMAKE_BUILD_EARLY_EXPANSION)
        return()
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/build-support/check_board_config.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/build-support/cmake/hosted_spi_diagnostics.cmake")
    cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL xiaotai_enable_hosted_spi_diagnostics)
    xiaotai_board_header("${manifest}" generated)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${generated}")
    return()
endif()

xiaotai_host_require(xiaotai_hal ${parts})
xiaotai_board_header("${manifest}" generated)
add_library(xiaotai_board STATIC board.cc)
target_include_directories(xiaotai_board PRIVATE "${generated}")
target_link_libraries(xiaotai_board PUBLIC xiaotai_hal PRIVATE ${parts})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
