# SPDX-License-Identifier: MIT
# Host tests of the bus component, on the fake devices, pins and PWM channels
# of host/bus_io.c.
xiaotai_host_test(part_bus SOURCES bus_test.c LINK xiaotai_part_bus)
xiaotai_host_test(part_bus_esp_i2c SOURCES esp_i2c_test.c ../esp-idf/i2c.c
    INCLUDE ../src esp_i2c_stubs INCLUDE_ONLY xiaotai_part_bus LINK xiaotai_port)
