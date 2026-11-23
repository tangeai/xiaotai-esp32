# SPDX-License-Identifier: MIT
# XiaoTai hardware abstraction: part descriptors and interfaces (xiaotai/hal.h),
# the board class (xiaotai/board.hpp) and the C view of the selected board. The
# product links exactly one board, which defines xiaotai::board::SelectedBoard().
# Below the runtime framework: only the port layer (which also publishes the
# hardware needs vocabulary, xiaotai/needs.h).
if(ESP_PLATFORM)
    idf_component_register(SRCS "src/hal.cc" INCLUDE_DIRS "include" REQUIRES xiaotai_port)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port)
add_library(xiaotai_hal STATIC src/hal.cc)
target_include_directories(xiaotai_hal PUBLIC include)
target_link_libraries(xiaotai_hal PUBLIC xiaotai_port)
target_compile_features(xiaotai_hal PUBLIC cxx_std_20)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
