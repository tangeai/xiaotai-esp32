#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regenerate or check the colour theme kit's drawn assets.

    python -B scripts/generate_lcd_theme_assets.py --check     # hashes only, no font tools
    python -B scripts/generate_lcd_theme_assets.py --write     # needs Pillow with FreeType

Three kinds of asset, all written to sdk/themes/xiaotai_theme_lcd_kit/assets:

  lcd_icons.c/.h     the kit's own line icons, drawn here from the geometry
                     below on a 24 unit grid and stored as two LVGL fonts
                     (20 px and 28 px, 4 bpp) in the private-use range
  lcd_font_num.c     digits for clocks, timers and codes (Noto Sans SC, 48, 64,
                     96 and 160 px, 4 bpp), every digit on the same advance
  lcd_font_cn_26.c   the text face of large displays: Noto Sans SC Medium,
                     26 px, 2 bpp: ASCII, the GB 2312 level-1 characters and
                     symbols, and every other character the applications'
                     fixed strings use. A character outside it is drawn by
                     the 18 px face (the font's fallback).

The icons are original drawings; nothing is traced or copied from an icon
set. The fonts come from the pinned Noto Sans SC file the 18 px face uses.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "sdk/themes/xiaotai_theme_lcd_kit/assets"
MANIFEST = ASSETS / "lcd_assets.json"
NOTO = ROOT / "components/xiaotai_lvgl/fonts/source/NotoSansSC[wght].ttf"
NOTO_SHA = "a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da"
LICENSE = "components/xiaotai_lvgl/fonts/source/OFL-NotoSansSC.txt"
FILES = ("lcd_icons.c", "lcd_icons.h", "lcd_font_num.c", "lcd_font_cn_26.c")
# Fixed strings are read from these trees (C string literals only).
STRING_TREES = ("apps", "sdk", "boards", "products")

# ------------------------------------------------------------------- icons

GRID = 24.0
STROKE = 2.2
ICON_SIZES = (20, 28)
ICON_BASE = 0xE000


def arc(cx, cy, r, start, end, steps=None):
    """Points on a circle; angles in degrees, 0 east, 90 south (y grows down)."""
    steps = steps or max(6, int(abs(end - start) / 6))
    return [(cx + r * math.cos(math.radians(start + (end - start) * i / steps)),
             cy + r * math.sin(math.radians(start + (end - start) * i / steps))) for i in range(steps + 1)]


def ring(cx, cy, r):
    return arc(cx, cy, r, 0, 360, 48)[:-1]


def rrect(x0, y0, x1, y1, r):
    return (arc(x1 - r, y0 + r, r, -90, 0, 6) + arc(x1 - r, y1 - r, r, 0, 90, 6)
            + arc(x0 + r, y1 - r, r, 90, 180, 6) + arc(x0 + r, y0 + r, r, 180, 270, 6))


def S(points, width=STROKE, closed=False):
    return ("stroke", list(points), width, closed)


def E(points, width):
    return ("erase", list(points), width, False)


def F(points):
    return ("fill", list(points))


def D(cx, cy, r):
    return ("fill", ring(cx, cy, r))


def gear():
    points = []
    for tooth in range(8):
        a = tooth * 45.0
        for radius, offset in ((7.4, -14.5), (9.8, -8.0), (9.8, 8.0), (7.4, 14.5)):
            points.append((12 + radius * math.cos(math.radians(a + offset)),
                           12 + radius * math.sin(math.radians(a + offset))))
    return points


SLASH = [E([(4.2, 3.8), (19.8, 20.2)], 5.6), S([(4.2, 3.8), (19.8, 20.2)])]
WIFI_DOT = D(12, 19, 1.7)
WIFI_ARCS = [S(arc(12, 19, r, 225, 315)) for r in (5.0, 9.5, 14.0)]
SPEAKER = S([(3.5, 9.5), (7.5, 9.5), (12.5, 5.5), (12.5, 18.5), (7.5, 14.5), (3.5, 14.5)], closed=True)
MIC = [S(rrect(9, 2.8, 15, 13.5, 3), closed=True), S(arc(12, 11, 6.6, 0, 180)), S([(12, 17.6), (12, 21)]),
       S([(8.5, 21), (15.5, 21)])]
VIDEO = [S(rrect(2.8, 6.5, 15.2, 17.5, 2.8), closed=True),
         S([(15.2, 10.6), (21.2, 7.2), (21.2, 16.8), (15.2, 13.4)], closed=True)]
HANDSET = [S(arc(19.5, 4.5, 13, 97, 173), 4.6), S(arc(19.5, 4.5, 13, 97, 116), 6.6),
           S(arc(19.5, 4.5, 13, 154, 173), 6.6)]
HANDSET_DOWN = [S(arc(12, 24, 13, 235, 305), 4.6), S(arc(12, 24, 13, 235, 252), 6.6),
                S(arc(12, 24, 13, 288, 305), 6.6)]

# name -> drawing; the order fixes the code points (ICON_BASE + index): append only.
ICONS = [
    ("back", [S([(14.5, 5.5), (8, 12), (14.5, 18.5)])]),
    ("next", [S([(9.5, 5.5), (16, 12), (9.5, 18.5)])]),
    ("down", [S([(5.5, 9), (12, 15.5), (18.5, 9)])]),
    ("more", [D(5, 12, 1.9), D(12, 12, 1.9), D(19, 12, 1.9)]),
    ("phone", HANDSET),
    ("hangup", HANDSET_DOWN),
    ("check", [S([(5, 12.5), (10, 17.5), (19.5, 7)])]),
    ("close", [S([(6, 6), (18, 18)]), S([(18, 6), (6, 18)])]),
    ("stop", [F(rrect(6.5, 6.5, 17.5, 17.5, 2.5))]),
    ("wifi", [WIFI_DOT] + WIFI_ARCS),
    ("wifi_2", [WIFI_DOT] + WIFI_ARCS[:2]),
    ("wifi_1", [WIFI_DOT] + WIFI_ARCS[:1]),
    ("wifi_0", [WIFI_DOT]),
    ("wifi_off", [WIFI_DOT] + WIFI_ARCS + SLASH),
    ("volume", [SPEAKER, S(arc(12.5, 12, 3.8, -50, 50)), S(arc(12.5, 12, 7.6, -50, 50))]),
    ("volume_off", [SPEAKER, S([(16.5, 9.5), (21.5, 14.5)]), S([(21.5, 9.5), (16.5, 14.5)])]),
    ("mic", MIC),
    ("mic_off", MIC + SLASH),
    ("video", VIDEO),
    ("video_off", VIDEO + SLASH),
    ("settings", [S(gear(), closed=True), S(ring(12, 12, 3.2), closed=True)]),
    ("refresh", [S(arc(12, 12, 8, 0, 300)), S([(16, 5.07), (19.5, 8.5)]), S([(19.5, 3.5), (19.5, 8.5), (14.5, 8.5)])]),
    ("lock", [S(rrect(5, 10.8, 19, 20.8, 2.5), closed=True), S([(8, 10.8)] + arc(12, 8, 4, 180, 360) + [(16, 10.8)])]),
    ("user", [S(ring(12, 8, 3.9), closed=True), S(arc(12, 21.5, 7.8, 198, 342))]),
    ("users", [S(ring(9, 8.5, 3.4), closed=True), S(arc(9, 21, 7, 200, 340)), S(arc(16.2, 8.5, 3.4, -75, 105)),
               S(arc(15.5, 21, 7, 300, 340))]),
    ("smile", [S(ring(12, 12, 9), closed=True), D(9, 10, 1.3), D(15, 10, 1.3), S(arc(12, 12.2, 4.6, 25, 155))]),
    ("sound", [S([(4, 10), (4, 14)]), S([(8, 6), (8, 18)]), S([(12, 9.5), (12, 14.5)]), S([(16, 4), (16, 20)]),
               S([(20, 10), (20, 14)])]),
    ("pulse", [S([(2.5, 12), (7.5, 12), (10.5, 5), (13.8, 19), (16.5, 12), (21.5, 12)])]),
    ("bell", [S(arc(12, 10.5, 6, 180, 360) + [(18, 14.5), (20, 18), (4, 18), (6, 14.5)], closed=True),
              S(arc(12, 19.2, 2, 20, 160))]),
    ("timer", [S(ring(12, 13.5, 7.6), closed=True), S([(9.5, 2.8), (14.5, 2.8)]), S([(12, 2.8), (12, 5.9)]),
               S([(12, 13.5), (12, 9.3)]), S([(12, 13.5), (15.2, 15.4)])]),
    ("clock", [S(ring(12, 12, 9), closed=True), S([(12, 6.8), (12, 12), (15.6, 14.2)])]),
    ("door", [S(rrect(6, 3, 18, 21, 1.5), closed=True), D(14.6, 12.5, 1.3), S([(2.8, 21), (21.2, 21)])]),
    ("key", [S(ring(7.8, 16.2, 4.3), closed=True), S([(10.9, 13.1), (20.3, 3.7)]), S([(15.6, 8.4), (18.6, 11.4)]),
             S([(18.4, 5.6), (20.6, 7.8)])]),
    ("plus", [S([(12, 5), (12, 19)]), S([(5, 12), (19, 12)])]),
    ("minus", [S([(5, 12), (19, 12)])]),
    ("backspace", [S([(8.5, 5), (20.5, 5), (20.5, 19), (8.5, 19), (2.8, 12)], closed=True),
                   S([(11.8, 9.3), (16.8, 14.7)]), S([(16.8, 9.3), (11.8, 14.7)])]),
    ("info", [S(ring(12, 12, 9), closed=True), D(12, 7.8, 1.4), S([(12, 11.4), (12, 16.8)])]),
    ("alert", [S([(12, 3.6), (21.6, 20), (2.4, 20)], closed=True), S([(12, 9.8), (12, 14.2)]), D(12, 17.1, 1.3)]),
    ("sun", [S(ring(12, 12, 4.4), closed=True)] + [S([(12 + r * math.cos(math.radians(t)), 12 + r * math.sin(math.radians(t)))
                                                      for r in (7.6, 10.2)]) for t in range(0, 360, 45)]),
    ("moon", [S(arc(12, 12, 8.6, 25, 245) + arc(16.6, 7.6, 7.2, 196, 73, 20), closed=True)]),
    ("calendar", [S(rrect(3.5, 5, 20.5, 20.5, 2.8), closed=True), S([(3.5, 10.2), (20.5, 10.2)]), S([(8, 2.8), (8, 6.6)]),
                  S([(16, 2.8), (16, 6.6)])]),
    ("leaf", [S(arc(19.5, 19.5, 15.5, 182, 268) + arc(4.5, 4.5, 15.5, 2, 88), closed=True), S([(5, 19), (14, 10)])]),
    ("call_out", [S([(6, 18), (18, 6)]), S([(9, 6), (18, 6), (18, 15)])]),
    ("call_in", [S([(18, 6), (6, 18)]), S([(15, 18), (6, 18), (6, 9)])]),
    ("call_missed", [S([(3, 8), (11, 16), (20.5, 6.5)]), S([(13.5, 6.5), (20.5, 6.5), (20.5, 13.5)])]),
    ("play", [F([(7.5, 4.8), (19, 12), (7.5, 19.2)]), S([(7.5, 4.8), (19, 12), (7.5, 19.2)], closed=True)]),
]

# ------------------------------------------------------------------- fonts

NUM_FACES = (("lcd_font_num_48", 48, 400), ("lcd_font_num_64", 64, 400), ("lcd_font_num_96", 96, 300),
             ("lcd_font_num_160", 160, 300))
NUM_CHARS = "0123456789:-. %/"
CN_NAME, CN_SIZE, CN_WEIGHT, CN_BPP, CN_LINE, CN_BASE = "lcd_font_cn_26", 26, 500, 2, 30, 6


def sha(data):
    return hashlib.sha256(data).hexdigest()


def level_one():
    """GB 2312 symbols (rows 1 and 3) and level-1 characters (rows 16 to 55)."""
    out = set()
    for row in (1, 3, *range(16, 56)):
        for cell in range(1, 95):
            try:
                out.add(ord(bytes((0xA0 + row, 0xA0 + cell)).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    return out


def fixed_text():
    """Every non-ASCII character of the C string literals under STRING_TREES."""
    chars = set()
    literal = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
    for tree in STRING_TREES:
        for path in sorted((ROOT / tree).rglob("*")):
            if path.suffix not in (".c", ".h", ".cc", ".inc", ".json") or "/assets/" in path.as_posix() \
                    or "/test" in path.as_posix() or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="ignore")
            for body in literal.findall(text):
                chars.update(ord(c) for c in body if ord(c) > 0x7F)
    return chars


def columns(items, count):
    return "\n".join("    " + ", ".join(items[i:i + count]) + "," for i in range(0, len(items), count))


def pack(alpha, bpp):
    levels = (1 << bpp) - 1
    values = [(v * levels + 127) // 255 for v in alpha]
    step = 8 // bpp
    values.extend([0] * (-len(values) % step))
    return bytes(sum(values[i + j] << (8 - (j + 1) * bpp) for j in range(step)) for i in range(0, len(values), step))


def font_c(name, glyphs, bpp, line_height, base_line, fallback=None):
    """glyphs: {code point: (advance_16ths, w, h, ofs_x, ofs_y, bitmap bytes)} -> C text of one lv_font_t."""
    points = sorted(glyphs)
    bitmap, rows = bytearray(), ["    {0},"]
    for cp in points:
        advance, w, h, x, y, payload = glyphs[cp]
        rows.append(f"    {{.bitmap_index = {len(bitmap)}, .adv_w = {advance}, .box_w = {w}, .box_h = {h}, "
                    f".ofs_x = {x}, .ofs_y = {y}}}, /* U+{cp:04X} */")
        bitmap.extend(payload)
    maps, lists, first = [], "", 1
    ascii_run = [cp for cp in points if cp < 0x80]
    if ascii_run and ascii_run == list(range(ascii_run[0], ascii_run[-1] + 1)):
        maps.append(f"    {{.range_start = {ascii_run[0]}, .range_length = {len(ascii_run)}, .glyph_id_start = 1,\n"
                    "     .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},")
        first += len(ascii_run)
        rest = [cp for cp in points if cp >= 0x80]
    else:
        rest = points
    if rest:
        start = rest[0]
        if rest[-1] - start > 0xFFFF:
            raise ValueError(f"{name}: code points span more than one sparse map")
        lists = (f"static const uint16_t {name}_list[] = {{\n" + columns([str(cp - start) for cp in rest], 14)
                 + "\n};\n")
        maps.append(f"    {{.range_start = {start}, .range_length = {rest[-1] - start + 1}, "
                    f".glyph_id_start = {first},\n     .unicode_list = {name}_list, .list_length = {len(rest)},\n"
                    "     .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY},")
    fallback_line = f"    .fallback = &{fallback},\n" if fallback else ""
    return f"""static const LV_ATTRIBUTE_LARGE_CONST uint8_t {name}_bitmap[] = {{
{columns([f'0x{b:02x}' for b in bitmap], 20)}
}};
static const lv_font_fmt_txt_glyph_dsc_t {name}_glyphs[] = {{
{chr(10).join(rows)}
}};
{lists}static const lv_font_fmt_txt_cmap_t {name}_cmaps[] = {{
{chr(10).join(maps)}
}};
static const lv_font_fmt_txt_dsc_t {name}_dsc = {{
    .glyph_bitmap = {name}_bitmap, .glyph_dsc = {name}_glyphs, .cmaps = {name}_cmaps,
    .cmap_num = {len(maps)}, .bpp = {bpp}, .bitmap_format = LV_FONT_FMT_TXT_PLAIN,
}};
const lv_font_t {name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {line_height}, .base_line = {base_line}, .subpx = LV_FONT_SUBPX_NONE,
{fallback_line}    .dsc = &{name}_dsc,
}};
""", len(bitmap), len(points)


def draw_icon(ops, size):
    from PIL import Image, ImageDraw

    over = 8
    scale = size * over / GRID
    image = Image.new("L", (size * over, size * over), 0)
    pen = ImageDraw.Draw(image)

    def at(point):
        return (point[0] * scale, point[1] * scale)

    for op in ops:
        if op[0] == "fill":
            pen.polygon([at(p) for p in op[1]], fill=255)
            continue
        kind, points, width, closed = op
        ink = 255 if kind == "stroke" else 0
        radius = width * scale / 2
        path = [at(p) for p in points] + ([at(points[0])] if closed else [])
        pen.line(path, fill=ink, width=max(1, round(width * scale)))
        for x, y in path:   # round caps and joins
            pen.ellipse((x - radius, y - radius, x + radius, y + radius), fill=ink)
    return image.resize((size, size), Image.Resampling.BOX)


def icon_glyphs(size):
    glyphs = {}
    for index, (name, ops) in enumerate(ICONS):
        image = draw_icon(ops, size)
        bounds = image.getbbox()
        if bounds is None:
            raise ValueError(f"icon {name}: nothing drawn")
        ink = image.crop(bounds)
        glyphs[ICON_BASE + index] = (size * 16, ink.width, ink.height, bounds[0], size - bounds[3],
                                     pack(ink.tobytes(), 4))
    return glyphs


def text_glyph(font, cp, bpp, line_height, base_line, advance=None):
    from PIL import Image

    natural = round(font.getlength(chr(cp)) * 16)
    advance = natural if advance is None else advance
    mask, (x, y) = font.getmask2(chr(cp), mode="L", anchor="ls")
    bounds = mask.getbbox()
    if bounds is None:
        return (advance, 0, 0, 0, 0, b"")
    ink = Image.frombytes("L", mask.size, bytes(mask)).crop(bounds)
    x, y = x + bounds[0] + (advance - natural) // 32, y + bounds[1]
    rows = min(ink.height, line_height)
    if rows != ink.height:
        ink = ink.resize((ink.width, rows), Image.Resampling.LANCZOS)
    # Keep every glyph inside the line box so no label clips it.
    y = min(max(base_line - line_height, y), base_line - rows)
    payload = pack(ink.tobytes(), bpp)
    if not any(payload):
        raise ValueError(f"U+{cp:04X}: ink vanished during quantization")
    return (advance, ink.width, rows, x, -y - rows, payload)


def generate():
    from PIL import ImageFont, __version__, features

    if sha(NOTO.read_bytes()) != NOTO_SHA:
        raise ValueError("the pinned Noto Sans SC file changed")
    out, facts = {}, {}

    names = [name for name, _ in ICONS]
    header = ["// SPDX-License-Identifier: MIT", "#pragma once",
              "/* The colour theme kit's own line icons, as UTF-8 code points of the fonts",
              " * lcd_icons_20 and lcd_icons_28. Generated by scripts/generate_lcd_theme_assets.py. */", ""]
    for index, name in enumerate(names):
        encoded = "".join(f"\\x{b:02X}" for b in chr(ICON_BASE + index).encode("utf-8"))
        header.append(f'#define LCD_ICON_{name.upper()} "{encoded}"')
    out["lcd_icons.h"] = "\n".join(header) + "\n"
    body = ["/* SPDX-License-Identifier: MIT",
            " * The colour theme kit's line icons: original drawings on a 24 unit grid with a",
            f" * {STROKE} unit round stroke, rendered at two sizes as 4 bpp LVGL fonts.",
            " * Generated by scripts/generate_lcd_theme_assets.py; edit the geometry there.", " */",
            '#include "lvgl.h"', ""]
    for size in ICON_SIZES:
        text, bitmap, count = font_c(f"lcd_icons_{size}", icon_glyphs(size), 4, size, 0)
        body.append(text)
        facts[f"lcd_icons_{size}"] = {"glyphs": count, "bitmap_bytes": bitmap}
    out["lcd_icons.c"] = "\n".join(body)

    notice = (f" * Noto Sans SC. Copyright 2014-2021 Adobe. Reserved Font Name: Source.\n"
              f" * Source SHA-256: {NOTO_SHA}\n * License: {LICENSE}\n"
              " * Generated by scripts/generate_lcd_theme_assets.py.\n */")
    body = ["/* SPDX-License-Identifier: OFL-1.1", " * Digits for clocks, timers and codes; every digit has the same advance.",
            notice, '#include "lvgl.h"', ""]
    for name, size, weight in NUM_FACES:
        font = ImageFont.truetype(str(NOTO), size, layout_engine=ImageFont.Layout.BASIC)
        font.set_variation_by_axes([weight])
        digit = max(round(font.getlength(c) * 16) for c in "0123456789")
        base = round(size * 0.14)
        line = round(size * 1.0)
        glyphs = {ord(c): text_glyph(font, ord(c), 4, line, base, digit if c.isdigit() else None) for c in NUM_CHARS}
        text, bitmap, count = font_c(name, glyphs, 4, line, base)
        body.append(text)
        facts[name] = {"glyphs": count, "bitmap_bytes": bitmap, "weight": weight}
    out["lcd_font_num.c"] = "\n".join(body)

    font = ImageFont.truetype(str(NOTO), CN_SIZE, layout_engine=ImageFont.Layout.BASIC)
    font.set_variation_by_axes([CN_WEIGHT])
    from fontTools.ttLib import TTFont
    with TTFont(NOTO, lazy=True) as sfnt:
        cmap = set(sfnt.getBestCmap())
    wanted = set(range(0x20, 0x7F)) | level_one() | fixed_text()
    missing = sorted(cp for cp in wanted if cp not in cmap)
    points = sorted(wanted & cmap)
    glyphs = {cp: text_glyph(font, cp, CN_BPP, CN_LINE, CN_BASE) for cp in points}
    text, bitmap, count = font_c(CN_NAME, glyphs, CN_BPP, CN_LINE, CN_BASE, fallback="ui_font_cn_18")
    out["lcd_font_cn_26.c"] = "\n".join([
        "/* SPDX-License-Identifier: OFL-1.1",
        f" * The text face of large displays: Medium ({CN_WEIGHT}), {CN_SIZE} px, {CN_BPP} bpp, {count} glyphs: ASCII,",
        " * GB 2312 symbols and level-1 characters, and the characters of the applications'",
        " * fixed strings. Anything else falls back to the complete 18 px face.",
        notice, '#include "lvgl.h"', "", "LV_FONT_DECLARE(ui_font_cn_18);", "", text])
    facts[CN_NAME] = {"glyphs": count, "bitmap_bytes": bitmap, "not_in_source": [f"U+{cp:04X}" for cp in missing]}

    import fontTools
    manifest = {"schema": 1, "noto_sha256": NOTO_SHA, "icons": names, "faces": facts,
                "tools": {"pillow": __version__, "freetype": features.version_module("freetype2"),
                          "fonttools": fontTools.__version__},
                "generated_sha256": {name: sha(text.encode("utf-8")) for name, text in out.items()}}
    return out, manifest


def check():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if manifest["schema"] != 1 or manifest["noto_sha256"] != NOTO_SHA:
        raise ValueError("lcd_assets.json does not match the reviewed inputs")
    if manifest["icons"] != [name for name, _ in ICONS]:
        raise ValueError("the icon list differs from lcd_assets.json: run --write")
    for name in FILES:
        if sha((ASSETS / name).read_bytes()) != manifest["generated_sha256"][name]:
            raise ValueError(f"{name}: generated asset hash differs")
    header = (ASSETS / "lcd_icons.h").read_text(encoding="utf-8")
    for name, _ in ICONS:
        if f"#define LCD_ICON_{name.upper()} " not in header:
            raise ValueError(f"lcd_icons.h has no {name}")
    # Fixed strings must stay drawable in the large face without the fallback.
    covered = set(int(cp, 16) for cp in re.findall(r"/\* U\+([0-9A-F]{4,6}) \*/",
                                                   (ASSETS / "lcd_font_cn_26.c").read_text(encoding="utf-8")))
    absent = set(int(cp[2:], 16) for cp in manifest["faces"][CN_NAME]["not_in_source"])
    uncovered = sorted(fixed_text() - covered - absent)
    if uncovered:
        raise ValueError("fixed strings use characters outside lcd_font_cn_26: "
                         + " ".join(f"U+{cp:04X}" for cp in uncovered[:20]) + ": run --write")
    for name, face in manifest["faces"].items():
        print(f"PASS {name}: {face['glyphs']} glyphs, {face['bitmap_bytes']} bitmap bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    task = parser.add_mutually_exclusive_group(required=True)
    task.add_argument("--check", action="store_true")
    task.add_argument("--write", action="store_true")
    args = parser.parse_args()
    if args.check:
        check()
        return
    out, manifest = generate()
    for name, text in out.items():
        (ASSETS / name).write_bytes(text.encode("utf-8"))
    MANIFEST.write_bytes((json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
    for name, face in manifest["faces"].items():
        print(f"{name}: {face['glyphs']} glyphs, {face['bitmap_bytes']} bitmap bytes")


if __name__ == "__main__":
    main()
