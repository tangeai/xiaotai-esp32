# SPDX-License-Identifier: MIT
# XiaoTai parts library: JD9165-class MIPI DSI panels in video mode
# (xiaotai/parts/panel_jd9165.h), as a panel driver that owns its scan-out
# frame buffers.
set(sources src/panel_jd9165.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} esp-idf/panel_io.c esp-idf/esp_lcd_jd9165.c
                           INCLUDE_DIRS "include"
                           PRIV_INCLUDE_DIRS "src" "esp-idf/include"
                           REQUIRES xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display
                           PRIV_REQUIRES esp_lcd esp_hw_support esp_driver_gpio hal)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display)
add_library(xiaotai_part_panel_jd9165 STATIC ${sources} host/panel_io.c)
target_include_directories(xiaotai_part_panel_jd9165 PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_part_panel_jd9165 PUBLIC xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
