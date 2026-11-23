# SPDX-License-Identifier: MIT
# XiaoTai parts library: the GT911 touch controller
# (xiaotai/parts/touch_gt911.h), as a touch driver.
set(sources src/touch_gt911.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} esp-idf/touch_io.c
                           INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display
                           PRIV_REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c esp_lcd_touch_gt911)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display)
add_library(xiaotai_part_touch_gt911 STATIC ${sources} host/touch_io.c)
target_include_directories(xiaotai_part_touch_gt911 PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_part_touch_gt911 PUBLIC xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
