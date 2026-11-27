# SPDX-License-Identifier: MIT
# XiaoTai parts library: output parts (xiaotai/parts/output.h) - an
# addressable RGB LED strip, and single on/off outputs (light, vibrator,
# relay) on a line.
set(sources src/output_line.c src/led_strip.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} esp-idf/led_rmt.c INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port xiaotai_hal xiaotai_part_bus PRIV_REQUIRES esp_driver_gpio esp_driver_rmt)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_hal xiaotai_part_bus)
add_library(xiaotai_part_output STATIC ${sources} host/led_io.c)
target_include_directories(xiaotai_part_output PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_part_output PUBLIC xiaotai_port xiaotai_hal xiaotai_part_bus)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
