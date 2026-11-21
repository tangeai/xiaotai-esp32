#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the locked GT911 poll body with the native adapter and instrumented IO."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
PART = HERE.parent
SDK = PART.parent.parent
GT911_LOCK = "737bdf29e0823eacf041381e81db2aaf5d378f3b5417df7f415f83319e3e790b"

LINK = r'''
static struct gt911_fixture controller;
esp_err_t esp_lcd_touch_new_i2c_gt911(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *cfg,
                                      esp_lcd_touch_handle_t *out)
{
    assert(cfg->rst_gpio_num == 23 && cfg->int_gpio_num == 21 && cfg->levels.reset);
    assert(((const esp_lcd_touch_io_gt911_config_t *)cfg->driver_data)->dev_addr == 0x5d);
    controller = (struct gt911_fixture){.io = io};
    *out = &controller;
    return ESP_OK;
}
esp_err_t esp_lcd_touch_del(esp_lcd_touch_handle_t touch)
{
    assert(touch == &controller);
    touch->io = NULL;
    return ESP_OK;
}
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t touch)
{
    return esp_lcd_touch_gt911_read_data(touch);
}
'''


def source_body(source, name):
    found = re.search(r"^static esp_err_t " + name + r"\([^;]*?\n\{.*?^\}", source, re.M | re.S)
    if found is None:
        raise ValueError(f"Missing pinned GT911 function {name}")
    return found[0]


def check_idf(idf):
    i2c = (idf / "components/esp_driver_i2c/include/driver/i2c_master.h").read_text(encoding="utf-8")
    panel = (idf / "components/esp_lcd/interface/esp_lcd_panel_io_interface.h").read_text(encoding="utf-8")
    for operation in ("i2c_master_transmit_receive", "i2c_master_multi_buffer_transmit"):
        assert re.search(r"\b" + operation + r"\([^;]+int xfer_timeout_ms\);", i2c)
    for method in ("tx_param", "rx_param", "del"):
        assert re.search(r"\(\*" + method + r"\)\(esp_lcd_panel_io_t \*io", panel)
    print("public bounded I2C and panel callbacks:", idf, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--idf", type=Path, nargs="+", required=True)
    args = parser.parse_args()
    assert (args.package / ".component_hash").read_text().strip() == GT911_LOCK
    checksums = json.loads((args.package / "CHECKSUMS.json").read_text())
    for entry in checksums["files"]:
        assert hashlib.sha256((args.package / entry["path"]).read_bytes()).hexdigest() == entry["hash"], entry["path"]
    for root in args.idf:
        check_idf(root)
    original = args.package / "esp_lcd_touch_gt911.c"
    source = original.read_text(encoding="utf-8")
    poll = [source_body(source, name) for name in (
        "touch_gt911_i2c_read", "touch_gt911_i2c_write", "esp_lcd_touch_gt911_read_data")]
    with tempfile.TemporaryDirectory(prefix="xt-gt911-poll-") as dir_name:
        temp = Path(dir_name)
        generated = temp / "poll.c"
        generated.write_text('#include "native_poll_shim.h"\n#include <assert.h>\n'
                             + "\n".join(poll) + "\n" + LINK, encoding="utf-8")
        for name in ("driver/gpio.h", "driver/i2c_master.h", "esp_lcd_panel_io_interface.h", "esp_lcd_touch_gt911.h"):
            header = temp / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.touch()
        includes = [HERE, temp, PART / "src", PART / "include", SDK / "xiaotai_hal/include",
                    SDK / "xiaotai_port/include", SDK / "parts/xiaotai_part_display/include",
                    SDK / "parts/xiaotai_part_bus/include"]
        binary = temp / "test"
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-include", HERE / "native_poll_shim.h",
                        *[f"-I{path}" for path in includes], generated, HERE / "native_poll_test.c",
                        PART / "esp-idf/touch_io.c", "-o", binary], check=True)
        subprocess.run([binary], check=True,
                       env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                            "UBSAN_OPTIONS": "halt_on_error=1"})
    print("GT911: one/empty/five-point/keys and failure poll sequences passed", flush=True)


if __name__ == "__main__":
    main()
