# SPDX-License-Identifier: MIT
"""Run the hardware encoder adapter contract with local public ESP-IDF headers."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def run(idf, video):
    root = video.parent.parent
    components = idf / "components"
    vendor = root / "vendor/esp32p4/espressif__esp_h264"
    includes = [video / "include", video / "src", root / "sdk/xiaotai_hal/include",
                root / "sdk/xiaotai_port/include", root / "sdk/xiaotai_port/host/include",
                vendor / "hw/include", vendor / "interface/include", components / "esp_mm/include",
                components / "esp_common/include"]
    sources = [video / name for name in ("test/video_h264_encoder_test.c",
                                        "esp-idf/encoder_h264_hw.c", "src/h264_au.c")]
    with tempfile.TemporaryDirectory(prefix="xiaotai-h264-native-") as directory:
        tmp = Path(directory)
        (tmp / "sdkconfig.h").write_text("#pragma once\n", encoding="ascii")
        binary = tmp / "video_h264_encoder_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                   *(f"-I{item}" for item in [tmp, *includes]), *sources, "-o", binary]
        subprocess.run(command, check=True)
        subprocess.run([binary], check=True, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        print(f"H.264 native adapter contract: {idf}: PASS", flush=True)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--idf", required=True, action="append", type=Path)
    args = p.parse_args()
    for path in args.idf:
        run(path, Path(__file__).resolve().parents[1])
