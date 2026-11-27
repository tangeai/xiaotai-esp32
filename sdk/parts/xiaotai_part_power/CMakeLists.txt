# SPDX-License-Identifier: MIT
# XiaoTai parts library: power-management and IO-expander chips on I2C
# (xiaotai/parts/power.h) - PCA9557, AW9523, AXP2101. They give other parts
# their lines and the power key.
set(sources src/pca9557.c src/aw9523.c src/axp2101.c src/power_io.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include"
                           REQUIRES xiaotai_port xiaotai_hal xiaotai_part_bus)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_hal xiaotai_part_bus)
add_library(xiaotai_part_power STATIC ${sources})
target_include_directories(xiaotai_part_power PUBLIC include)
target_link_libraries(xiaotai_part_power PUBLIC xiaotai_port xiaotai_hal xiaotai_part_bus)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
