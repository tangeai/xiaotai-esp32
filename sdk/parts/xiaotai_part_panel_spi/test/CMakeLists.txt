# SPDX-License-Identifier: MIT
xiaotai_host_test(part_panel_spi SOURCES panel_spi_test.c INCLUDE ../host
                  LINK xiaotai_part_panel_spi xiaotai_part_panel_ili9341 xiaotai_part_panel_st7796)
target_link_options(part_panel_spi PRIVATE "LINKER:--wrap=xiaotai_delay_ms")
if(DEFINED ENV{XIAOTAI_SPI_NATIVE_TEST_IDF})
    set(native_idfs "$ENV{XIAOTAI_SPI_NATIVE_TEST_IDF}")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME part_panel_spi_native_ownership
             COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/native_spi_overlay_test.py"
                     --idf ${native_idfs})
    set_tests_properties(part_panel_spi_native_ownership PROPERTIES LABELS xiaotai_part_panel_spi TIMEOUT 120)
endif()
