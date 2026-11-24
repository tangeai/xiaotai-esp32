# SPDX-License-Identifier: MIT
# XiaoTai parts library: command-less panels on a parallel RGB bus
# (xiaotai/parts/panel_rgb.h), as a panel driver that owns its scan-out frame
# buffers.
set(sources src/panel_rgb.c)
if(ESP_PLATFORM)
    # The DMA low-level headers stop the scan-out before a restart; IDF 6 moved
    # them from the common hal component into esp_hal_dma.
    set(priv esp_lcd)
    idf_build_get_property(idf_path IDF_PATH)
    if(EXISTS "${idf_path}/components/esp_hal_dma")
        list(APPEND priv esp_hal_dma)
    endif()
    idf_component_register(SRCS ${sources} esp-idf/panel_io.c
                           INCLUDE_DIRS "include"
                           PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display
                           PRIV_REQUIRES ${priv})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display)
add_library(xiaotai_part_panel_rgb STATIC ${sources} host/panel_io.c)
target_include_directories(xiaotai_part_panel_rgb PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_part_panel_rgb PUBLIC xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
