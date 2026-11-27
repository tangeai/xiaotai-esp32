#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Moving pictures of the colour themes' face, on the computer.

    python3 tools/face_preview.py --out build/face-preview                  # every scene, 320 x 240 displays
    python3 tools/face_preview.py --scene talk --size 320x165 --size 600x310
    python3 tools/face_preview.py --stills --size 240x124                    # one picture of every expression

Builds the host test theme_lcd_face_life (Linux or WSL, the same host build as
tests/sdk/run.py), lets it draw each scene frame by frame with the firmware's
own code, in as many pixels as the display shows the face in, and writes one
animated WebP per scene and size: without loss and without scaling, so what is
seen is what the display shows. --gif writes GIFs as well (fewer colours). The
face takes 240x124 pixels of a 320x240 display, 320x165 of 480x320, 600x310 of
1024x600. The scenes are in sdk/themes/xiaotai_theme_lcd_kit/test/face_life_test.c.
Needs Pillow.
"""

import argparse
import array
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCENES = ("idle", "talk", "touch", "states", "moods")
TARGET = "theme_lcd_face_life"


def build(directory):
    configure = ["cmake", "-S", str(ROOT / "tests/sdk"), "-B", directory, "-DXIAOTAI_COMPONENTS=xiaotai_theme_lcd_kit",
                 "-DXIAOTAI_BOARD=", "-DXIAOTAI_HOST_DEPENDENCY_TESTS=OFF", "-DXIAOTAI_HOST_ALLOW_MISSING=OFF",
                 f"-DXIAOTAI_HOST_PYTHON={sys.executable}"]
    subprocess.run(configure, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", directory, "--target", TARGET, "--parallel", "8"], check=True, stdout=subprocess.DEVNULL)
    return next(path for path in Path(directory).rglob(TARGET) if path.is_file())


def pictures(raw, width, height, zoom):
    from PIL import Image
    pixels = array.array("H", raw)
    if sys.byteorder == "big":
        pixels.byteswap()
    # What an RGB565 panel shows of each value.
    shown = [bytes(((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)) for v in range(65536)]
    size = width * height
    for at in range(0, len(pixels), size):
        picture = Image.frombytes("RGB", (width, height), b"".join(map(shown.__getitem__, pixels[at:at + size])))
        yield picture if zoom == 1 else picture.resize((width * zoom, height * zoom), Image.NEAREST)


def measure(text):
    width, _, height = text.partition("x")
    return int(width), int(height)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=ROOT / "build/face-preview")
    parser.add_argument("--scene", action="append", choices=SCENES)
    parser.add_argument("--size", action="append", type=measure, metavar="WxH", help="pixels of the face; 240x124 unless given")
    parser.add_argument("--ms", type=int, default=20, help="between frames")
    parser.add_argument("--zoom", type=int, default=1, help="whole multiples, pixel by pixel")
    parser.add_argument("--stills", action="store_true", help="one PNG of every expression instead")
    parser.add_argument("--gif", action="store_true")
    parser.add_argument("--program", type=Path, help="an already built " + TARGET)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="xiaotai-face-") as directory:
        program = args.program or build(directory)
        for width, height in args.size or [(240, 124)]:
            name = f"{width}x{height}"
            if args.stills:
                raw = Path(directory) / "stills"
                subprocess.run([str(program), "stills", "-", str(raw), str(width), str(height), "1500"], check=True)
                for index, picture in enumerate(pictures(raw.read_bytes(), width, height, args.zoom)):
                    picture.save(args.out / f"{name}-{index:02d}.png", optimize=True)
                continue
            for scene in args.scene or SCENES:
                raw = Path(directory) / scene
                subprocess.run([str(program), "film", scene, str(raw), str(width), str(height), str(args.ms)], check=True)
                frames = list(pictures(raw.read_bytes(), width, height, args.zoom))
                target = args.out / f"{scene}-{name}.webp"
                frames[0].save(target, save_all=True, append_images=frames[1:], duration=args.ms, loop=0, lossless=True, quality=60, method=3)
                if args.gif:
                    reduced = [frame.quantize(96) for frame in frames]
                    reduced[0].save(target.with_suffix(".gif"), save_all=True, append_images=reduced[1:], duration=args.ms, loop=0)
                print(f"{target}: {len(frames)} frames, {target.stat().st_size // 1024} KB")


if __name__ == "__main__":
    main()
