# SPDX-License-Identifier: MIT
include("${CMAKE_CURRENT_LIST_DIR}/../../../sdk/cmake/xiaotai_board.cmake")
set(manifest "${CMAKE_CURRENT_LIST_DIR}/board.json")
set(parts xiaotai_part_audio xiaotai_part_bus xiaotai_part_camera_dvp xiaotai_part_display
          xiaotai_part_panel_ili9341 xiaotai_part_panel_spi xiaotai_part_power xiaotai_part_touch_ft5x06)
if(ESP_PLATFORM)
    idf_component_register(SRCS "${CMAKE_CURRENT_LIST_DIR}/board.cc"
                           REQUIRES xiaotai_hal PRIV_REQUIRES ${parts} WHOLE_ARCHIVE)
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
