#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Derive a private SPI LCD factory with error-safe result accounting."""
import argparse
import hashlib
from pathlib import Path
import re
import sys

SOURCE = "spi/esp_lcd_panel_io_spi.c"
PRIVATE_HEADER = "priv_include/esp_lcd_common.h"
PINS = {
    "9adca37bbe631ae28b3f6761a3a30dab9b30620ed3bbf10ec6186cfe9f8d5d77":
        "bbff1d2b72f1f6b255112557a45fef28ce3be5d652541f1203fdc403225b085c",  # IDF 5.5.5
    "0e9b223dc6828209f91b4ceeb754d04f2e4ea15caa373997e54075f85109be3e":
        "ebd7c7dd3473c456acad6cbcb7b0dc4323f3eef2598fcfdb76d9502a7f85f242",  # IDF 6.0.3
}


def replace(text, old, new, expected=1):
    if text.count(old) != expected:
        raise ValueError(f"Unexpected pinned source shape: {old!r}")
    return text.replace(old, new)


def normal(path):
    return path.read_bytes().replace(b"\r\n", b"\n")


def generate(lcd, build):
    lcd = lcd.resolve(strict=True)
    idf = lcd.parent.parent
    if build.is_symlink() or build.resolve().is_relative_to(idf):
        raise ValueError("Output must be outside the pristine ESP-IDF tree")
    original = normal(lcd / SOURCE)
    digest = hashlib.sha256(original).hexdigest()
    if PINS.get(digest) != hashlib.sha256(normal(lcd / PRIVATE_HEADER)).hexdigest():
        raise ValueError(f"Unreviewed pinned ESP-IDF SPI LCD source/header: {digest}")
    source = original.decode("utf-8")
    source = replace(source, "esp_err_t esp_lcd_new_panel_io_spi(", "esp_err_t xiaotai_lcd_new_panel_io_spi(")
    source = replace(source, '#include "esp_lcd_common.h"\n',
                     '#include "esp_lcd_common.h"\n#include "spi_lcd_ownership.h"\n')
    source = replace(source, "} esp_lcd_panel_io_spi_t;\n",
                     '} esp_lcd_panel_io_spi_t;\n\n#include "spi_lcd_ownership.inc"\n')
    # Keep the vendor's caller/descriptor/callback shapes. The four drain
    # sites now consume all completed descriptors, even after the first fault.
    loop = re.compile(
        r"(?m)^(?P<i> +)size_t num_trans_inflight = spi_panel_io->num_trans_inflight;\n"
        r"(?P=i)for \(size_t i = 0; i < num_trans_inflight; i\+\+\) \{\n"
        r"(?P=i)    ret = spi_device_get_trans_result\(spi_panel_io->spi_dev, &spi_trans, portMAX_DELAY\);\n"
        r'(?P=i)    ESP_GOTO_ON_ERROR\(ret, err, TAG, "recycle spi transactions failed"\);\n'
        r"(?P=i)    spi_panel_io->num_trans_inflight--;\n(?P=i)\}"
    )
    source, count = loop.subn(
        r"\g<i>ret = xiaotai_spi_drain(spi_panel_io);\n"
        r'\g<i>ESP_GOTO_ON_ERROR(ret, err, TAG, "recycle spi transactions failed");', source)
    if count != 4:
        raise ValueError(f"Expected four ESP-IDF drain sites, found {count}")
    source = replace(source, "    spi_transaction_t *spi_trans = NULL;\n", "", 4)
    color_start = "static esp_err_t panel_io_spi_tx_color(esp_lcd_panel_io_t *io, int lcd_cmd, const void *color, size_t color_size)\n{\n"
    source = replace(source, color_start, color_start + "    spi_transaction_t *spi_trans;\n")
    source = replace(source, "ret = spi_device_get_trans_result(spi_panel_io->spi_dev, &spi_trans, portMAX_DELAY);",
                     "ret = xiaotai_spi_reap(spi_panel_io, &spi_trans);")
    source = replace(source, "            spi_panel_io->num_trans_inflight--;\n", "")
    source = replace(source, "    spi_bus_remove_device(spi_panel_io->spi_dev);\n",
                     '    ret = spi_bus_remove_device(spi_panel_io->spi_dev);\n'
                     '    ESP_GOTO_ON_ERROR(ret, err, TAG, "remove spi device failed");\n')
    source = replace(source, " */\n\n#include <stdlib.h>",
                     " */\n\n/* XiaoTai derivative: ownership of consumed SPI results. */\n#include <stdlib.h>")

    def write(path, content):
        if path.is_symlink():
            raise ValueError(f"Refusing symlink output {path}")
        if not path.exists() or path.read_bytes() != content:
            path.write_bytes(content)

    build.mkdir(parents=True, exist_ok=True)
    output = build / "xiaotai_lcd_spi.c"
    write(build / "LICENSE-APACHE-2.0", normal(idf / "LICENSE"))
    write(output, source.encode("utf-8"))
    return output.resolve()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lcd", type=Path)
    parser.add_argument("build", type=Path)
    args = parser.parse_args()
    try:
        print(generate(args.lcd, args.build))
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
