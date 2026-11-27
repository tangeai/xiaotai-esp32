# SPDX-License-Identifier: MIT
xiaotai_host_test(part_panel_ssd1306 SOURCES panel_ssd1306_test.c LINK xiaotai_part_panel_ssd1306 INCLUDE ../host)
xiaotai_host_test(part_bounded_lcd_i2c SOURCES bounded_lcd_i2c_test.c ../esp-idf/bounded_lcd_i2c.c
                  INCLUDE lcd_stubs ../esp-idf)
