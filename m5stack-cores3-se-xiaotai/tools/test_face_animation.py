#!/usr/bin/env python3
"""Run the local face rasterizer checks with host cc, without ESP-IDF or devices."""
from pathlib import Path
import os
import re
import subprocess
import sys
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
        str(root / "tools/test_face_animation.c"), str(lvgl / "misc/lv_math.c"), str(lvgl / "misc/lv_area.c"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary), *sys.argv[1:]], check=True)
    header = (root / "components/starter_product/include/starter_product.h").read_text()
    stats = re.search(r"typedef struct \{[^}]*\} starter_product_display_stats_t;", header).group()
    harness = (root / "tools/test_face_animation.c").read_text()
    harness = harness.replace(
        'static int64_t esp_timer_get_time(void) { return host_ms * 1000; }',
        'static unsigned host_timer_calls;\n'
        'static int64_t esp_timer_get_time(void) { return host_ms * 1000 + host_timer_calls++ % 100; }')
    harness = harness.replace('#include "starter_product_s3_face.inc"',
        '#define CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE 1\n' + stats + '\n'
        'static starter_product_display_stats_t s_display_stats = {\n'
        '    .face_clear_us = UINT32_MAX, .face_raster_us = UINT32_MAX};\n'
        '#include "starter_product_s3_face.inc"')
    harness = harness.replace('    s3_face_frame(NULL);\n    assert(!s3_face.clear_all);',
        '    uint64_t clear_before = s_display_stats.face_clear_us;\n'
        '    uint64_t raster_before = s_display_stats.face_raster_us;\n'
        '    s3_face_frame(NULL);\n'
        '    assert(s_display_stats.face_clear_us == clear_before + 1);\n'
        '    assert(s_display_stats.face_raster_us == raster_before + 1);\n'
        '    assert(!s3_face.clear_all);')
    # Avoid a wrapped fake clock while retaining deterministic stage deltas.
    harness = harness.replace('    host_ms += 40;', '    host_ms += 40; host_timer_calls = 0;')
    generated = Path(directory) / "timed_face.c"
    generated.write_text(harness)
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-DLV_CONF_SKIP", "-DLV_COLOR_DEPTH=16",
        "-DLV_COLOR_16_SWAP=1", "-DLV_COLOR_MIX_ROUND_OFS=128",
        "-I", str(lvgl), "-I", str(product), str(generated),
        str(lvgl / "misc/lv_math.c"), str(lvgl / "misc/lv_area.c"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary), *sys.argv[1:]], check=True)
    print("PASS: development raster stage boundaries, 64-bit totals, unchanged face/damage checks")
