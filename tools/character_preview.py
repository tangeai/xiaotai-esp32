#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the on-screen character on the computer: every mood and state as PNG,
every reaction and touch as an animated PNG, plus an overview page.

The pictures come from the code the device runs (sdk/themes/xiaotai_theme_character):
its portable core is compiled with the host C compiler and stepped frame by
frame, so no board is needed to see what the character looks like or how it
moves. Animated PNG plays in every current browser.

    python3 tools/character_preview.py --out build/character-preview
    python3 tools/character_preview.py --out out --size 1024x600 --only mood,state
"""

import argparse
import html
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "sdk" / "themes" / "xiaotai_theme_character"
SOURCES = ("src/chr_raster.c", "src/chr_mind.c", "src/chr_draw.c", "test/chr_gallery.c")
GROUPS = (("mood", "表情（主题接受的 23 个情绪名，和对讲的 4 个）"), ("word", "对讲里的十个情绪词"), ("state", "对讲阶段和设备状态"), ("talk", "一次完整对讲"),
          ("cue", "事件反应和待机小动作"), ("touch", "触摸互动"), ("life", "连续场景"))
FRAME_MS = 40


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)


def packed(rgb, width, top, rows):
    stride = width * 3
    return zlib.compress(b"".join(b"\0" + rgb[y * stride:(y + 1) * stride] for y in range(top, top + rows)), 6)


def write_png(path, width, height, rgb):
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", packed(rgb, width, 0, height)) + chunk(b"IEND", b""))


def write_apng(path, width, height, frames):
    """frames: raw RGB frames of one size. Rows that did not change are not stored again."""
    stride = width * 3
    parts = []          # [top, rows, data, delay in frames]
    previous = None
    for rgb in frames:
        if previous is None:
            parts.append([0, height, packed(rgb, width, 0, height), 1])
        elif rgb == previous:
            parts[-1][3] += 1
        else:
            top = next(y for y in range(height) if rgb[y * stride:(y + 1) * stride] != previous[y * stride:(y + 1) * stride])
            bottom = next(y for y in range(height - 1, -1, -1)
                          if rgb[y * stride:(y + 1) * stride] != previous[y * stride:(y + 1) * stride])
            parts.append([top, bottom - top + 1, packed(rgb, width, top, bottom - top + 1), 1])
        previous = rgb
    out = [b"\x89PNG\r\n\x1a\n", chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)),
           chunk(b"acTL", struct.pack(">II", len(parts), 0))]
    sequence = 0
    for index, (top, rows, data, delay) in enumerate(parts):
        out.append(chunk(b"fcTL", struct.pack(">IIIIIHHBB", sequence, width, rows, 0, top, delay * FRAME_MS, 1000, 0, 0)))
        sequence += 1
        if index == 0:
            out.append(chunk(b"IDAT", data))
        else:
            out.append(chunk(b"fdAT", struct.pack(">I", sequence) + data))
            sequence += 1
    out.append(chunk(b"IEND", b""))
    path.write_bytes(b"".join(out))


def build(work):
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        raise SystemExit("character_preview: no host C compiler (cc, gcc or clang) on PATH; run it on Linux or WSL")
    binary = work / "chr_gallery"
    command = [compiler, "-O2", "-std=gnu11", f"-I{COMPONENT / 'src'}", *[str(COMPONENT / name) for name in SOURCES],
               "-lm", "-o", str(binary)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f"character_preview: the gallery did not compile\n{result.stderr}")
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, type=Path, help="folder for the pictures and index.html")
    parser.add_argument("--size", default="760x600", help="stage size, e.g. 1024x600 (default 760x600)")
    parser.add_argument("--only", default="", help="comma-separated groups: " + ",".join(name for name, _ in GROUPS))
    args = parser.parse_args()
    width, height = (int(value) for value in args.size.lower().split("x"))
    only = {name for name in args.only.split(",") if name}
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="chr-preview-") as folder:
        work = Path(folder)
        binary = build(work)
        raw = work / "raw"
        raw.mkdir()
        subprocess.run([str(binary), str(raw), str(COMPONENT / "assets" / "character.pack"), str(width), str(height)], check=True)
        rows = [line.split("\t") for line in (raw / "index.tsv").read_text(encoding="utf-8").splitlines()]
        shown = []
        for group, name, title, w, h, count in rows:
            if only and group not in only:
                continue
            w, h, count = int(w), int(h), int(count)
            data = (raw / f"{group}-{name}.rgb").read_bytes()
            size = w * h * 3
            target = args.out / f"{group}-{name}.png"
            if count == 1:
                write_png(target, w, h, data[:size])
            else:
                write_apng(target, w, h, [data[i * size:(i + 1) * size] for i in range(count)])
            shown.append((group, name, title, w, h, count))
            print(f"{target.name}: {w}x{h}, {count} frame(s), {target.stat().st_size // 1024} KB", flush=True)
    sections = []
    for group, heading in GROUPS:
        cards = "".join(
            f'<figure><img loading="lazy" src="{group}-{name}.png" width="{w if count > 1 else w // 2}" alt="{html.escape(title)}">'
            f"<figcaption>{html.escape(title)}{'' if count == 1 else f'（{count * FRAME_MS / 1000:.1f} 秒）'}</figcaption></figure>"
            for g, name, title, w, h, count in shown if g == group)
        if cards:
            sections.append(f"<h2>{html.escape(heading)}</h2><div class=grid>{cards}</div>")
    (args.out / "index.html").write_text(
        "<!doctype html><meta charset=utf-8><title>人物预览</title><style>"
        "body{background:#101315;color:#f2f5f7;font:15px/1.5 system-ui,sans-serif;margin:24px}"
        "h1{font-size:22px}h2{font-size:17px;color:#5ec8f2;margin-top:28px}"
        ".grid{display:flex;flex-wrap:wrap;gap:12px}figure{margin:0;background:#1c232a;border-radius:10px;padding:8px}"
        "img{display:block;border-radius:6px;max-width:100%}figcaption{color:#98a3ad;padding-top:6px;text-align:center}"
        "</style><h1>屏幕人物预览</h1><p>图片由设备上运行的同一份代码在电脑上逐帧画出；动图是 APNG，浏览器里直接播放。</p>"
        + "".join(sections), encoding="utf-8")
    print(f"Overview: {args.out / 'index.html'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
