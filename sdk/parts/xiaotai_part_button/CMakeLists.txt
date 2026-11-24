# SPDX-License-Identifier: MIT
# XiaoTai parts library: a push button on a line as an input part, and the
# debounce it uses (xiaotai/parts/button.h).
set(sources src/button.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include"
                           REQUIRES xiaotai_port xiaotai_hal xiaotai_part_bus)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_hal xiaotai_part_bus)
add_library(xiaotai_part_button STATIC ${sources})
target_include_directories(xiaotai_part_button PUBLIC include)
target_link_libraries(xiaotai_part_button PUBLIC xiaotai_port xiaotai_hal xiaotai_part_bus)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
