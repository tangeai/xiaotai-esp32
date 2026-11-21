# SPDX-License-Identifier: MIT
# XiaoTai parts library: what the parts of a board share (xiaotai/parts/bus.h)
# - the I2C bus, on/off lines (a pin, an expander bit, a power rail) and
# 0..100 % levels (PWM).
#
# Portable (src/): the bus object with its lock and the locked
# read-modify-write (i2c.c), pin lines and levels (line.c). The platform does
# the smallest wire actions behind src/bus_io.h:
#   esp-idf/i2c.c, gpio.c, pwm.c   I2C master, GPIO and LEDC drivers
#   host/bus_io.c                  fake I2C devices, pins and PWM channels that
#                                  tests of this and every part built on it
#                                  program through host/include/xiaotai_bus_host.h
set(sources src/i2c.c src/line.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} "esp-idf/i2c.c" "esp-idf/gpio.c" "esp-idf/pwm.c"
                           INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port
                           PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_driver_ledc)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port)
add_library(xiaotai_part_bus STATIC ${sources} host/bus_io.c)
target_include_directories(xiaotai_part_bus PUBLIC include host/include PRIVATE src)
target_link_libraries(xiaotai_part_bus PUBLIC xiaotai_port)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
