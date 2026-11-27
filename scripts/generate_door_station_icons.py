#!/usr/bin/env python3
"""Draw the door's extra icons and write them as LVGL 9 data.

The shapes follow Lucide's 24-unit line icons (stroke 2, round caps; ISC
licence, apps/xiaotai_app_door/assets/ICONS_LICENSE.txt). They are drawn with
Pillow at 8x and reduced, so no SVG renderer is needed. Output:
apps/xiaotai_app_door/assets/door_station_icons.c and .h. `--check` compares
the checked-in files with a fresh rendering. The users icon at 36 px keeps the
RGB565A8 form of the retained icons; the others are coverage only (A8), which
the templates draw in their ink.
"""
from __future__ import annotations

import argparse
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "apps/xiaotai_app_door/assets/door_station_icons.c"
HEADER = OUT.with_suffix(".h")
SCALE = 8


class Pen:
    """Lucide's 24-unit canvas: lines, arcs and circles with round caps."""

    def __init__(self, draw, unit, width):
        self.draw, self.unit, self.width = draw, unit, width

    def line(self, *points):
        scaled = [(x * self.unit, y * self.unit) for x, y in points]
        self.draw.line(scaled, fill=255, width=self.width, joint="curve")
        r = self.width / 2
        for x, y in scaled:
            self.draw.ellipse([x - r, y - r, x + r, y + r], fill=255)

    def arc(self, cx, cy, r, start, end, ry=None):
        ry = r if ry is None else ry
        half = self.width / 2 / self.unit
        box = [(cx - r - half) * self.unit, (cy - ry - half) * self.unit,
               (cx + r + half) * self.unit, (cy + ry + half) * self.unit]
        self.draw.arc(box, start, end, fill=255, width=self.width)

    def dot(self, x, y, r):
        u = self.unit
        self.draw.ellipse([(x - r) * u, (y - r) * u, (x + r) * u, (y + r) * u], fill=255)


def _users(draw, unit, width):
    """Lucide "users": a person in front, the shoulder and head of another behind."""
    def p(x, y):
        return (x * unit, y * unit)

    def line(a, b):
        draw.line([p(*a), p(*b)], fill=255, width=width)
        for x, y in (a, b):
            r = width / 2
            draw.ellipse([x * unit - r, y * unit - r, x * unit + r, y * unit + r], fill=255)

    def arc(cx, cy, r, start, end):
        box = [p(cx - r, cy - r), p(cx + r, cy + r)]
        draw.arc([box[0][0], box[0][1], box[1][0], box[1][1]], start, end, fill=255, width=width)

    arc(9, 7, 4, 0, 360)                       # front head
    line((16, 21), (16, 19))                   # front shoulders
    arc(12, 19, 4, 270, 360)
    line((12, 15), (6, 15))
    arc(6, 19, 4, 180, 270)
    line((2, 19), (2, 21))
    line((22, 21), (22, 19))                   # the person behind
    arc(18, 19, 4, 284, 360)
    start = math.degrees(math.atan2(3.13 - 7, 16 - 15.008)) % 360
    arc(15.008, 7, 4, start, 360 - start)


def _bell(draw, unit, width):
    pen = Pen(draw, unit, width)
    pen.arc(12, 8, 6, 180, 360)
    pen.line((6, 8), (5.8, 11), (5.2, 13.4), (4.2, 15.4), (3, 17), (21, 17), (19.8, 15.4), (18.8, 13.4), (18.2, 11), (18, 8))
    pen.arc(12, 20, 2, 20, 160)


def _eye(draw, unit, width):
    pen = Pen(draw, unit, width)
    top = [(2 + i * 2, 12 - 6.6 * math.sin(math.pi * i / 10)) for i in range(11)]
    pen.line(*top)
    pen.line(*[(x, 24 - y) for x, y in top])
    pen.arc(12, 12, 3, 0, 360)


def _settings(draw, unit, width):
    pen = Pen(draw, unit, width)
    for a, b in (((21, 4), (14, 4)), ((10, 4), (3, 4)), ((21, 12), (12, 12)), ((8, 12), (3, 12)), ((21, 20), (16, 20)),
                 ((12, 20), (3, 20)), ((14, 2), (14, 6)), ((8, 10), (8, 14)), ((16, 18), (16, 22))):
        pen.line(a, b)


def _history(draw, unit, width):
    pen = Pen(draw, unit, width)
    pen.arc(12, 12, 9, 0, 360)
    pen.line((12, 7), (12, 12), (15.5, 14))


def _next(draw, unit, width):
    Pen(draw, unit, width).line((9, 18), (15, 12), (9, 6))


def _previous(draw, unit, width):
    Pen(draw, unit, width).line((15, 18), (9, 12), (15, 6))


def _refresh(draw, unit, width):
    pen = Pen(draw, unit, width)
    pen.arc(12, 12, 8.5, 190, 330)
    pen.arc(12, 12, 8.5, 10, 150)
    pen.line((20.5, 3), (20.5, 8), (15.5, 8))
    pen.line((3.5, 21), (3.5, 16), (8.5, 16))


def _switch(draw, unit, width):
    pen = Pen(draw, unit, width)
    pen.line((8, 3), (4, 7), (8, 11))
    pen.line((4, 7), (20, 7))
    pen.line((16, 21), (20, 17), (16, 13))
    pen.line((20, 17), (4, 17))


def _mute(draw, unit, width):
    """A bell that is struck out: the visitor is not answered."""
    _bell(draw, unit, width)
    Pen(draw, unit, width).line((3, 3), (21, 21))


def _house(draw, unit, width):
    pen = Pen(draw, unit, width)
    pen.line((2.5, 11), (12, 3), (21.5, 11))
    pen.line((5, 9.2), (5, 21), (19, 21), (19, 9.2))
    pen.line((9.5, 21), (9.5, 14), (14.5, 14), (14.5, 21))


def _door(draw, unit, width):
    pen = Pen(draw, unit, width)
    pen.line((6, 21), (6, 3), (18, 3), (18, 21))
    pen.line((2, 21), (22, 21))
    pen.dot(14.5, 12.5, 1.4)


# name, size, drawing, RGB565A8
ICONS = [
    ("users_36", 36, _users, True), ("users_24", 24, _users, False), ("bell_36", 36, _bell, False),
    ("bell_24", 24, _bell, False), ("eye_36", 36, _eye, False), ("eye_24", 24, _eye, False),
    ("settings_24", 24, _settings, False), ("history_36", 36, _history, False), ("next_24", 24, _next, False),
    ("previous_24", 24, _previous, False), ("refresh_36", 36, _refresh, False), ("switch_36", 36, _switch, False),
    ("mute_36", 36, _mute, False), ("house_36", 36, _house, False), ("door_36", 36, _door, False),
]


def render(draw_fn, size):
    from PIL import Image, ImageDraw

    big = size * SCALE
    mask = Image.new("L", (big, big), 0)
    unit = big / 24.0
    draw_fn(ImageDraw.Draw(mask), unit, int(round(2 * unit * 0.75)))
    return mask.resize((size, size), Image.LANCZOS)


def c_source() -> str:
    lines = ["// SPDX-License-Identifier: MIT",
             "// Generated by scripts/generate_door_station_icons.py; do not edit.",
             "// Lucide line icons (ISC), see ICONS_LICENSE.txt. RGB565A8: RGB565 plane, then A8 plane.",
             '#include "door_station_icons.h"', ""]
    for short, size, fn, colour in ICONS:
        name = f"door_asset_{short}"
        alpha = list(render(fn, size).tobytes())
        data = ([0xFF, 0xFF] * (size * size) if colour else []) + alpha
        form, stride = ("RGB565A8", size * 2) if colour else ("A8", size)
        lines.append(f"static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {name}_pixels[] = {{")
        for i in range(0, len(data), 24):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
        lines.append("};")
        lines.append(f"const lv_image_dsc_t {name} = {{")
        lines.append(f"    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_{form}, "
                     f".w = {size}, .h = {size}, .stride = {stride}}},")
        lines.append(f"    .data_size = sizeof({name}_pixels), .data = {name}_pixels,")
        lines.append("};")
        lines.append("")
    return "\n".join(lines)


def c_header() -> str:
    return "\n".join(["// SPDX-License-Identifier: MIT", "#pragma once",
                      "/* The door's icons drawn by scripts/generate_door_station_icons.py. */", '#include "lvgl.h"', ""]
                     + [f"LV_IMAGE_DECLARE(door_asset_{short});" for short, *_ in ICONS]) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    texts = {OUT: c_source(), HEADER: c_header()}
    if args.check:
        for path, text in texts.items():
            if path.read_text(encoding="utf-8") != text:
                raise SystemExit(f"{path} is stale: run python scripts/generate_door_station_icons.py")
        print("PASS: door station icons match their drawing")
        return
    for path, text in texts.items():
        path.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
