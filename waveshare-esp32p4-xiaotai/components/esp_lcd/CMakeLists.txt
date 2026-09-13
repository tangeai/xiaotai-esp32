# ESP-IDF 5.5.4 does not expose the SPI LCD PSRAM-direct DMA option that was
# added later on the upstream release/v5.5 branch. Keep the backport isolated
# here while reusing the rest of the exact ESP-IDF component sources.
idf_build_get_property(target IDF_TARGET)

if(${target} STREQUAL "linux")
    return()
endif()

set(idf_lcd_dir "$ENV{IDF_PATH}/components/esp_lcd")
file(TO_CMAKE_PATH "${idf_lcd_dir}" idf_lcd_dir)
set(srcs "${idf_lcd_dir}/src/esp_lcd_common.c"
         "${idf_lcd_dir}/src/esp_lcd_panel_io.c"
         "${idf_lcd_dir}/src/esp_lcd_panel_nt35510.c"
         "${idf_lcd_dir}/src/esp_lcd_panel_ssd1306.c"
         "${idf_lcd_dir}/src/esp_lcd_panel_st7789.c"
         "${idf_lcd_dir}/src/esp_lcd_panel_ops.c")
set(includes "include" "${idf_lcd_dir}/include" "${idf_lcd_dir}/interface")
set(priv_requires "esp_mm" "esp_psram" "esp_pm" "esp_driver_i2s")
set(public_requires "driver" "esp_driver_gpio" "esp_driver_i2c" "esp_driver_spi")

if(CONFIG_SOC_DMA2D_SUPPORTED)
    list(APPEND srcs "${idf_lcd_dir}/src/esp_async_fbcpy.c")
endif()

if(CONFIG_SOC_I2C_SUPPORTED)
    list(APPEND srcs "${idf_lcd_dir}/i2c/esp_lcd_panel_io_i2c_v1.c"
                     "${idf_lcd_dir}/i2c/esp_lcd_panel_io_i2c_v2.c")
endif()

if(CONFIG_SOC_GPSPI_SUPPORTED)
    list(APPEND srcs "spi/esp_lcd_panel_io_spi.c")
endif()

if(CONFIG_SOC_I2S_SUPPORTS_LCD_CAMERA)
    list(APPEND srcs "${idf_lcd_dir}/i80/esp_lcd_panel_io_i2s.c")
endif()

if(CONFIG_SOC_PARLIO_SUPPORT_SPI_LCD)
    list(APPEND srcs "${idf_lcd_dir}/parl/esp_lcd_panel_io_parl.c")
endif()

if(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED)
    list(APPEND srcs "${idf_lcd_dir}/i80/esp_lcd_panel_io_i80.c")
endif()

if(CONFIG_SOC_LCDCAM_RGB_LCD_SUPPORTED)
    list(APPEND includes "${idf_lcd_dir}/rgb/include")
    list(APPEND srcs "${idf_lcd_dir}/rgb/esp_lcd_panel_rgb.c")
endif()

if(CONFIG_SOC_MIPI_DSI_SUPPORTED)
    list(APPEND includes "${idf_lcd_dir}/dsi/include")
    list(APPEND srcs "${idf_lcd_dir}/dsi/esp_lcd_mipi_dsi_bus.c"
                     "${idf_lcd_dir}/dsi/esp_lcd_panel_io_dbi.c"
                     "${idf_lcd_dir}/dsi/esp_lcd_panel_dpi.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${includes}
                       PRIV_INCLUDE_DIRS "${idf_lcd_dir}/priv_include"
                       PRIV_REQUIRES ${priv_requires}
                       REQUIRES ${public_requires}
                       LDFRAGMENTS "${idf_lcd_dir}/linker.lf")
