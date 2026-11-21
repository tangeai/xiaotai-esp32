# SPDX-License-Identifier: MIT
function(xiaotai_spi_configure_native_overlay component)
    idf_component_get_property(lcd esp_lcd COMPONENT_DIR)
    idf_build_get_property(python PYTHON)
    if(NOT python)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(python "${Python3_EXECUTABLE}")
    endif()
    set(here "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    execute_process(
        COMMAND "${python}" -B "${here}/native_spi_overlay.py" "${lcd}"
                "${CMAKE_CURRENT_BINARY_DIR}/xiaotai_spi_native"
        RESULT_VARIABLE status OUTPUT_VARIABLE generated ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT status STREQUAL "0")
        message(FATAL_ERROR "Cannot prepare the pinned SPI LCD ownership adapter: ${error}")
    endif()
    file(TO_CMAKE_PATH "${generated}" generated)
    target_sources("${component}" PRIVATE "${generated}")
    target_include_directories("${component}" PRIVATE "${here}" "${lcd}/priv_include")
    target_compile_definitions("${component}" PRIVATE XIAOTAI_SPI_NATIVE_CORRECTION=1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${here}/native_spi_overlay.py" "${here}/spi_lcd_ownership.inc"
        "${lcd}/spi/esp_lcd_panel_io_spi.c" "${lcd}/priv_include/esp_lcd_common.h"
        "${lcd}/../../LICENSE")
endfunction()
