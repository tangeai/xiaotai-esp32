#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw every screen of a product on the computer and save it as PNG.

The real product, board package, theme and LVGL run on the host (Linux or
WSL), the same build the product host tests use. The scenario taps through
the pages it can reach and plays an AI talk and a device call, then this tool
converts the frames to PNG and writes an overview page.

    python3 tools/ui_preview.py                       # the four colour products
    python3 tools/ui_preview.py --product xiaotai-szpi-s3 --out build/ui-preview
"""

import argparse
import html
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
PRODUCT_TESTS = ROOT / "tests" / "sdk" / "product"
sys.path.insert(0, str(PRODUCT_TESTS))
import run as product_run  # noqa: E402

DEFAULT_PRODUCTS = ("xiaotai-szpi-s3", "xiaotai-cores3-lite", "xiaotai-waveshare-p4-rev3",
                    "xiaotai-wt99p461-s1-1v1")
SCENES = {"home": "screens", "setup": "screens-setup"}


def read_ppm(path):
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    if parts[0] != b"P6" or parts[2] != b"255":
        raise ValueError(f"not a P6 picture: {path}")
    width, height = map(int, parts[1].split())
    return width, height, parts[3][:width * height * 3]


def write_png(path, width, height, rgb):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def build(product, build_root, jobs, log):
    cmake = shutil.which("cmake")
    cache = build_root / product
    sources = product_run.locked_sources([product])
    generator = ["-G", "Ninja"] if shutil.which("ninja") and not (cache / "CMakeCache.txt").exists() else []
    env = {**os.environ, "LC_ALL": "C"}
    for command in ([cmake, "-S", PRODUCT_TESTS, "-B", cache, *generator, *sources,
                     f"-DXIAOTAI_HOST_PRODUCT={product}", "-DXIAOTAI_HOST_PRODUCT_CASE=default",
                     "-DXIAOTAI_HOST_PRODUCT_START_ONLY=OFF", "-DXIAOTAI_HOST_SANITIZE=OFF",
                     "-DXIAOTAI_HOST_ALLOW_MISSING=OFF"],
                    [cmake, "--build", cache, "--parallel", str(jobs), "--target", "product_host"]):
        if product_run.step(command, log, env):
            raise RuntimeError(f"{product}: build failed, see {log}")
    return cache / "product_host"


def shoot(executable, product, out, log):
    pictures = []
    for scene, scenario in SCENES.items():
        folder = out / product / scene
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir(parents=True)
        env = {**os.environ, "XIAOTAI_SCREENS_DIR": str(folder), "ASAN_OPTIONS": "detect_leaks=0"}
        with log.open("a", encoding="utf-8") as stream:
            code = subprocess.run([executable, scenario], cwd=folder, stdout=stream, stderr=subprocess.STDOUT,
                                  env=env, timeout=300).returncode
        if code:
            print(f"WARN: {product} {scenario} exited {code}; keeping the pictures taken so far", flush=True)
        index = folder / "index.tsv"
        rows = index.read_text(encoding="utf-8").splitlines() if index.exists() else []
        for row in rows:
            shot, name, page, controls = (row.split("\t") + ["", "", "", ""])[:4]
            ppm = folder / f"{shot}-{name}.ppm"
            if not ppm.exists():
                continue
            width, height, rgb = read_ppm(ppm)
            png = ppm.with_suffix(".png")
            write_png(png, width, height, rgb)
            ppm.unlink()
            pictures.append({"scene": scene, "name": name, "page": page, "controls": controls,
                             "file": png.relative_to(out).as_posix(), "width": width, "height": height})
    return pictures


def overview(out, results):
    parts = ["<!doctype html><meta charset=utf-8><title>小钛界面预览</title><style>"
             "body{font:14px system-ui,sans-serif;margin:24px;background:#f4f5f7;color:#1d2129}"
             "h2{margin:32px 0 12px}figure{display:inline-block;margin:0 16px 16px 0;vertical-align:top;"
             "background:#fff;padding:8px;border-radius:8px;box-shadow:0 1px 3px #0002}"
             "img{display:block;image-rendering:pixelated}figcaption{margin-top:6px;max-width:320px;"
             "font-size:12px;color:#4e5969}</style><h1>小钛界面预览</h1>"]
    for product, pictures in results.items():
        parts.append(f"<h2>{html.escape(product)}</h2>")
        for picture in pictures:
            parts.append(f"<figure><img src='{html.escape(picture['file'])}' width={picture['width']} "
                         f"height={picture['height']}><figcaption><b>{html.escape(picture['name'])}</b> "
                         f"· {html.escape(picture['page'])}</figcaption></figure>")
    (out / "index.html").write_text("".join(parts), encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--product", action="append", help="product name; repeat for several (default: colour products)")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "ui-preview")
    parser.add_argument("--build-root", type=Path, help="keep host builds here (default: <out>/host-build)")
    parser.add_argument("-j", "--jobs", type=int, default=min(8, os.cpu_count() or 1))
    args = parser.parse_args(argv)
    if os.name != "posix" or not shutil.which("cmake"):
        parser.error("the preview runs the product host build: use Linux or WSL with CMake")
    out = args.out.resolve()
    build_root = (args.build_root or out / "host-build").resolve()
    out.mkdir(parents=True, exist_ok=True)
    build_root.mkdir(parents=True, exist_ok=True)
    results = {}
    for product in args.product or DEFAULT_PRODUCTS:
        log = out / f"{product}.log"
        log.write_text("", encoding="utf-8")
        try:
            executable = build(product, build_root, args.jobs, log)
            results[product] = shoot(executable, product, out, log)
        except (RuntimeError, subprocess.TimeoutExpired, OSError, ValueError) as error:
            print(f"FAIL: {error}", file=sys.stderr, flush=True)
            results[product] = []
        print(f"{product}: {len(results[product])} pictures", flush=True)
    overview(out, results)
    print(f"Overview: {out / 'index.html'}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
