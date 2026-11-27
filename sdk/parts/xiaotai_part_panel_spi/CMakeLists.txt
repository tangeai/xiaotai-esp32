# SPDX-License-Identifier: MIT
# XiaoTai parts library: colour panels on a 4-wire SPI bus as panel drivers
# (xiaotai/parts/panel_spi.h), with the ST7789 controller; other controllers
# are components that add a controller object.
set(sources src/panel_spi.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} esp-idf/spi_io.c esp-idf/dma_alignment.c
                           INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS src
                           REQUIRES xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display
                           PRIV_REQUIRES esp_lcd esp_driver_gpio esp_driver_spi esp_mm)
    if(NOT CMAKE_BUILD_EARLY_EXPANSION AND CONFIG_SOC_PSRAM_DMA_CAPABLE AND CONFIG_SPIRAM)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_SPI_EXTERNAL_DMA=1)
        if(CONFIG_XIAOTAI_PART_PANEL_SPI_DMA_OWNERSHIP_FIX)
            include(esp-idf/native_spi_overlay.cmake)
            xiaotai_spi_configure_native_overlay(${COMPONENT_LIB})
        endif()
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display)
add_library(xiaotai_part_panel_spi STATIC ${sources} host/spi_io.c)
target_include_directories(xiaotai_part_panel_spi PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_part_panel_spi PUBLIC xiaotai_port xiaotai_hal xiaotai_part_bus xiaotai_part_display)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
