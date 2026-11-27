# SPDX-License-Identifier: MIT
# XiaoTai parts library: the ST7796 SPI panel controller
# (xiaotai/parts/panel_st7796.h) as a controller object for
# xiaotai_part_panel_spi.
if(ESP_PLATFORM)
    idf_component_register(SRCS esp-idf/controller.c INCLUDE_DIRS "include"
                           REQUIRES xiaotai_part_panel_spi
                           PRIV_REQUIRES esp_lcd espressif__esp_lcd_st7796)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_part_panel_spi)
add_library(xiaotai_part_panel_st7796 STATIC host/controller.c)
target_include_directories(xiaotai_part_panel_st7796 PUBLIC include)
target_link_libraries(xiaotai_part_panel_st7796 PUBLIC xiaotai_part_panel_spi)
