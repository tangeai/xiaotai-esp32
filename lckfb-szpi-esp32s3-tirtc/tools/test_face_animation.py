#!/usr/bin/env python3
"""Run the local face rasterizer checks with host cc, without ESP-IDF or devices."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
lvgl = root / "managed_components/lvgl__lvgl/src"
product = root / "components/starter_product/src"
if not (lvgl / "misc/lv_math.c").is_file():
    raise SystemExit("Local LVGL sources are required; this tool does not download dependencies.")
with tempfile.TemporaryDirectory(prefix="xiaotai-face-") as directory:
    binary = Path(directory) / "test_face"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-DLV_CONF_SKIP", "-DLV_COLOR_DEPTH=16",
        "-DLV_COLOR_16_SWAP=1", "-DLV_COLOR_MIX_ROUND_OFS=128",
        "-I", str(lvgl), "-I", str(product),
        str(root / "tools/test_face_animation.c"), str(lvgl / "misc/lv_math.c"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
