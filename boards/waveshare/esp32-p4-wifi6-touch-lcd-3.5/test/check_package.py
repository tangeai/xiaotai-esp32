#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the compiled inventory and native build contract against the board package."""

import csv
import hashlib
import json
import subprocess
import sys
from pathlib import Path


def partition_rows(path):
    with path.open(encoding="utf-8", newline="") as source:
        return [[cell.strip() for cell in row] for row in csv.reader(
            line for line in source if line.strip() and not line.lstrip().startswith("#")
        )]


cmake, manifest, executable, root = sys.argv[1:]
manifest = Path(manifest)
board = manifest.parent
description = json.loads(manifest.read_text(encoding="utf-8"))
actual = json.loads(subprocess.check_output([executable], text=True))
assert description["parts"] == actual, (description["parts"], actual)
assert partition_rows(board / description["partitions"]) == partition_rows(board / "test/legacy_partitions.csv")

# The approved C6 image the legacy P4 application embedded (same bytes, same hash).
embedded = (board / "assets/waveshare_c6_app.bin").read_bytes()
assert len(embedded) == 1_111_904
assert hashlib.sha256(embedded).hexdigest() == "8700a7215c77ffc9810ab27dbf7687d6222c620c13d7814dc502ace6a784b4f5"

raw10 = "CAMERA_OV5647_MIPI_RAW10_1280X960_BINNING_45FPS"
raw10_default = "CAMERA_OV5647_MIPI_DEFAULT_FMT_RAW10_1280X960_BINNING_45FPS"
format_index = "CAMERA_OV5647_MIPI_IF_FORMAT_INDEX_DEFAULT"
camera_contract = "OV5647 RAW10 1280x960 at 45 fps"
defaults = dict(line.removeprefix("CONFIG_").split("=", 1)
                for line in (board / "sdkconfig.board").read_text(encoding="utf-8").splitlines()
                if line.startswith("CONFIG_"))
config = {
    "IDF_TARGET": "esp32p4", "ESP32P4_SELECTS_REV_LESS_V3": "n", "ESP32P4_REV_MIN_FULL": "301",
    "ESP32P4_REV_MAX_FULL": "399", "ESP_DEFAULT_CPU_FREQ_MHZ": "400", "HAL_WDT_USE_ROM_IMPL": "n",
    "ESP_HOSTED_CP_TARGET_ESP32C6": "y", "ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO": "y",
    "ESP_HOSTED_HOST_SDIO_CLK_KHZ": "40000",
    raw10: defaults.get(raw10, "n"), raw10_default: defaults.get(raw10_default, "n"),
    format_index: "4",
}
cases = [
    ({}, None),
    ({"ESP32P4_SELECTS_REV_LESS_V3": "y"}, "rev3.1-3.99"),
    ({"ESP32P4_REV_MIN_FULL": "100"}, "rev3.1-3.99"),
    ({"ESP32P4_REV_MAX_FULL": "499"}, "rev3.1-3.99"),
    ({"ESP_DEFAULT_CPU_FREQ_MHZ": "360"}, "400 MHz"),
    ({"HAL_WDT_USE_ROM_IMPL": "y"}, "non-ROM WDT"),
    ({"ESP_HOSTED_CP_TARGET_ESP32C6": "n"}, "SDR SDIO"),
    ({"ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO": "n"}, "SDR SDIO"),
    ({"ESP_HOSTED_HOST_SDIO_CLK_KHZ": "50000"}, "SDR SDIO"),
    ({"ESP_HOSTED_HOST_SDIO_CLK_KHZ": "0"}, "SDR SDIO"),
    ({raw10: "n"}, camera_contract),
    ({raw10_default: "n", "CAMERA_OV5647_MIPI_DEFAULT_FMT_RAW8_800X800_50FPS": "y",
      format_index: "2"}, camera_contract),
    ({format_index: "2"}, camera_contract),
]
for overrides, expected in cases:
    args = [f"-DCONFIG_{key}={value}" for key, value in (config | overrides).items()]
    run = subprocess.run([cmake, *args, "-P", board / "build-support/check_board_config.cmake"],
                         text=True, capture_output=True, check=False)
    if expected is None:
        assert run.returncode == 0, run.stderr
    else:
        assert run.returncode and expected in run.stderr, (overrides, run.stderr)
