# SPDX-License-Identifier: MIT
# XiaoTai parts library: the ILI9341 family of SPI panel controllers
# (xiaotai/parts/panel_ili9341.h) as a controller object for
# xiaotai_part_panel_spi.
if(ESP_PLATFORM)
    idf_component_register(SRCS esp-idf/controller.c INCLUDE_DIRS "include"
                           REQUIRES xiaotai_part_panel_spi
                           PRIV_REQUIRES esp_lcd espressif__esp_lcd_ili9341)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_part_panel_spi)
add_library(xiaotai_part_panel_ili9341 STATIC host/controller.c)
target_include_directories(xiaotai_part_panel_ili9341 PUBLIC include)
target_link_libraries(xiaotai_part_panel_ili9341 PUBLIC xiaotai_part_panel_spi)
