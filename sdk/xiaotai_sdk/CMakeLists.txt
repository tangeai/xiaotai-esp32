# SPDX-License-Identifier: MIT
# XiaoTai SDK public API (include/xiaotai/xiaotai.h): xiaotai_start() runs the
# generated product glue, initializes the board and starts the runtime
# framework with the generated tables (include/xiaotai/selection.h). It links
# no optional engine: what a product runs is decided by the build's selection.
set(sources src/xiaotai.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include"
                           REQUIRES xiaotai_port xiaotai_framework xiaotai_hal)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_framework xiaotai_hal)
add_library(xiaotai_sdk STATIC ${sources})
target_include_directories(xiaotai_sdk PUBLIC include)
target_link_libraries(xiaotai_sdk PUBLIC xiaotai_port xiaotai_framework xiaotai_hal)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
