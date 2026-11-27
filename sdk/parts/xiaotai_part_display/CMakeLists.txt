# SPDX-License-Identifier: MIT
# XiaoTai parts library: the display part (xiaotai/parts/display.h), assembled
# from a panel driver, an optional touch driver and a backlight; owns power-up
# order, rollback and the touch filter.
set(sources src/display.c src/touch_filter.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include"
                           REQUIRES xiaotai_port xiaotai_hal xiaotai_part_bus)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_hal xiaotai_part_bus)
add_library(xiaotai_part_display STATIC ${sources})
target_include_directories(xiaotai_part_display PUBLIC include)
target_link_libraries(xiaotai_part_display PUBLIC xiaotai_port xiaotai_hal xiaotai_part_bus)
target_compile_features(xiaotai_part_display PUBLIC c_std_11)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
