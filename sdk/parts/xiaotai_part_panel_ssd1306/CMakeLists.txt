# SPDX-License-Identifier: MIT
# XiaoTai parts library: SSD1306-class monochrome panels on I2C with bounded
# transfers (xiaotai/parts/panel_ssd1306.h), as a panel driver.
set(sources src/panel_ssd1306.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} esp-idf/panel_io.c esp-idf/bounded_lcd_i2c.c
                           INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display
                           PRIV_REQUIRES esp_lcd esp_driver_i2c)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display)
add_library(xiaotai_part_panel_ssd1306 STATIC ${sources} host/panel_io.c)
target_include_directories(xiaotai_part_panel_ssd1306 PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_part_panel_ssd1306 PUBLIC xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
