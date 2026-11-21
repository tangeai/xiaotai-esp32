# SPDX-License-Identifier: MIT
"""Check native video decoder adapters using prepared ESP-IDF public headers."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def run(idf: Path, video: Path, alignment: int) -> None:
    root = video.parent.parent
    native = idf / "components"
    vendor = root / "vendor/esp32p4/espressif__esp_h264"
    includes = [video / "include", root / "sdk/xiaotai_hal/include", root / "sdk/xiaotai_port/include",
                root / "sdk/xiaotai_port/host/include", vendor / "interface/include", vendor / "sw/include",
                native / "esp_driver_jpeg/include", native / "esp_common/include", native / "hal/include",
                native / "soc/esp32p4/include", native / "soc/include"]
    if (native / "esp_hal_jpeg/include").is_dir():
        includes.append(native / "esp_hal_jpeg/include")
    with tempfile.TemporaryDirectory(prefix="xiaotai-decoder-native-") as directory:
        tmp = Path(directory)
        (tmp / "sdkconfig.h").write_text(
            f"#pragma once\n#define CONFIG_CACHE_L1_CACHE_LINE_SIZE 64\n"
            f"#define CONFIG_CACHE_L2_CACHE_LINE_SIZE {alignment}\n", encoding="ascii")
        binary = tmp / "video_decoders_native_test"
        files = [video / name for name in ("esp-idf/decoder_h264_sw.c", "esp-idf/decoder_jpeg_hw.c",
                                          "test/video_decoders_native_test.c")]
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                   *(f"-I{path}" for path in [tmp, *includes]), *files, "-o", binary]
        subprocess.run(command, check=True)
        subprocess.run([binary], check=True, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        print(f"Video decoder native API: {idf}, alignment={alignment}: PASS", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", required=True, action="append", type=Path)
    args = parser.parse_args()
    for path in args.idf:
        for align in (64, 128):
            run(path, Path(__file__).resolve().parents[1], align)
