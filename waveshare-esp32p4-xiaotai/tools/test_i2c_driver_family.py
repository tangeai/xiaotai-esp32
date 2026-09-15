#!/usr/bin/env python3
"""Exercise the P4 link gate, including constructors hidden from public symbols."""
from check_i2c_driver_family import check_family

check_family("40010000 T i2c_new_master_bus\n", "esp_driver_i2c/libesp_driver_i2c.a(i2c_master.c.obj)")
check_family("40010000 T i2c_driver_install\n", "")
for symbols, link_map in (
    ("40010000 T i2c_driver_install\n40020000 T i2c_new_master_bus\n", ""),
    ("40010000 T i2c_new_master_bus\n", "driver/libdriver.a(i2c.c.obj)"),
    ("", "driver\\libdriver.a(i2c.c.obj)"),
    ("", "esp_lcd_panel_io_i2c_v1.c.obj"),
):
    try:
        check_family(symbols, link_map)
    except ValueError:
        continue
    raise AssertionError((symbols, link_map))
print("PASS: I2C family, legacy constructors and Windows map paths")
