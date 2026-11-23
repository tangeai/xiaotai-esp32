# SPDX-License-Identifier: MIT
"""Compile the PPA adapter on the host against prepared ESP-IDF public headers."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def run(idf: Path, video: Path, alignment: int) -> None:
    root = video.parent
    native = idf / "components"
    includes = [video / "include", video / "src", root / "xiaotai_hal/include",
                root / "xiaotai_port/include", root / "xiaotai_port/host/include",
                native / "esp_driver_ppa/include", native / "esp_common/include",
                native / "esp_mm/include", native / "hal/include"]
    hal = native / "esp_hal_ppa/include"
    if hal.is_dir():
        includes.append(hal)
    if not (native / "esp_driver_ppa/include/driver/ppa.h").is_file():
        raise SystemExit(f"No prepared PPA header at {idf}")
    with tempfile.TemporaryDirectory(prefix="xiaotai-ppa-native-") as directory:
        tmp = Path(directory)
        (tmp / "sdkconfig.h").write_text(
            f"#pragma once\n#define CONFIG_CACHE_L1_CACHE_LINE_SIZE 64\n"
            f"#define CONFIG_CACHE_L2_CACHE_LINE_SIZE {alignment}\n", encoding="ascii")
        binary = tmp / "video_ppa_test"
        cc = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
              "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
        for path in [tmp, *includes]:
            cc += ["-I", str(path)]
        files = [video / name for name in ("esp-idf/transform_ppa.c", "src/geometry.c", "test/video_ppa_test.c")]
        subprocess.run([*cc, *files, "-o", binary], check=True)
        subprocess.run([binary], check=True, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        print(f"PPA native API: {idf}, alignment={alignment}: PASS", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", required=True, action="append", type=Path, help="local ESP-IDF tree (repeatable)")
    args = parser.parse_args()
    video = Path(__file__).resolve().parents[1]
    for path in args.idf:
        for alignment in (64, 128):
            run(path, video, alignment)
