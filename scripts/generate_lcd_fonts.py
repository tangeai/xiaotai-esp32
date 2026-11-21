#!/usr/bin/env python3
"""Regenerate or check the shared LVGL 18 px assets with pinned offline inputs."""

import argparse
import hashlib
import itertools
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "components/xiaotai_lvgl/fonts"
FONT_DIR = "components/xiaotai_lvgl/fonts/source"
NOTO = f"{FONT_DIR}/NotoSansSC[wght].ttf"
LICENSE = f"{FONT_DIR}/OFL-NotoSansSC.txt"
NOTO_SHA = "a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da"
BASE = "ui_font_cn_18"
SUPPLEMENT = BASE + "_supplement"
NAMES = (BASE, SUPPLEMENT)
METRICS = {BASE: (2, 20, 3), SUPPLEMENT: (4, 20, 3)}
OMIT = {0x2252}
RETAIN = {
    0x02DA, 0x0648, 0x0669, 0x0E05, 0x0E07, 0x0E51, 0x0EB6, 0x0F0D, 0x0F0E, 0x10DA,
    0x1107, 0x1109, 0x11BA, 0x11BD, 0x141B, 0x141F, 0x1420, 0x1452, 0x146D,
    0x1555, 0x1559, 0x15D2, 0x15D5, 0x15DC, 0x15E3, 0x15E8, 0x1D17, 0x1D25,
    0x1D54, 0x1D55, 0x203F, 0x25D5, 0x2727, 0xB208,
}
OLD_SOURCE = {
    "revision": "095bef11b83b1687ee7f45a6a003f610fc2560c4",
    BASE: "979aad8574ed907904ec0414582a832fd1320a986f823146bdc2d9ef582d86c0",
    SUPPLEMENT: "16c418f1b51ff8aab4ab955ea7ebd531de545c196f348d328b6bf5931f79e48a",
    "supplement_input_sha256": "f47eb4a9a151ccd51b5f0cfab2551ea0673b699d17b12741a00fa980f1ca99da",
}
FIELDS = ("bitmap_index", "adv_w", "box_w", "box_h", "ofs_x", "ofs_y")
# Latin letters, digits and ASCII punctuation use Noto's own proportional
# advances. The old face set every one of them to a fixed 9 px cell, which put
# wide gaps after narrow glyphs ("Xi aoTai", "06: 15"). CJK keeps 18 px.
NATURAL = range(0x20, 0x7F)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def field(text, name):
    return int(re.search(rf"\.{name}\s*=\s*(-?\d+)", text)[1])


def block(text, name):
    return re.search(rf"\b{name}\[\]\s*=\s*\{{(.*?)\n\}};", text, re.S)[1]


def read_asset(text):
    """Read the two cmap forms that these LVGL assets actually use."""
    ranges = block(text, "cmaps")
    glyphs = [line for line in block(text, "glyph_dsc").splitlines() if "{" in line]
    bitmap = re.sub(r"/\*.*?\*/", "", block(text, "glyph_bitmap"), flags=re.S)
    pixels = bytes(int(v, 16) for v in re.findall(r"0x[0-9a-f]{1,2}\b", bitmap, re.I))
    bpp, height, baseline = (field(text, key) for key in ("bpp", "line_height", "base_line"))
    by_id = {}
    for record in re.findall(r"\{([^{}]+)\}", ranges):
        start, length, gid = (field(record, key) for key in ("range_start", "range_length", "glyph_id_start"))
        if "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY" in record:
            offsets = range(length)
        elif "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY" in record:
            name = re.search(r"\.unicode_list\s*=\s*(\w+)", record)[1]
            offsets = [int(i, 0) for i in re.findall(r"0x[\da-fA-F]+|\d+", block(text, name))]
            if len(offsets) != field(record, "list_length") or offsets != sorted(set(offsets)):
                raise ValueError("invalid sparse cmap")
            if max(offsets) >= length:
                raise ValueError("sparse cmap exceeds its range")
        else:
            raise ValueError("unexpected cmap type")
        for i, offset in enumerate(offsets, gid):
            if i in by_id:
                raise ValueError("overlapping glyph IDs")
            by_id[i] = start + offset
    if sorted(by_id) != list(range(1, len(glyphs))) or len(set(by_id.values())) != len(by_id):
        raise ValueError("cmap does not account for every non-reserved glyph")
    values = {}
    cursor = 0
    for i in range(1, len(glyphs)):
        index, advance, w, h, x, y = (field(glyphs[i], key) for key in FIELDS)
        end = index + (w * h * bpp + 7) // 8
        if index != cursor or end > len(pixels):
            raise ValueError("bitmap offsets do not match packed data")
        values[by_id[i]] = [advance, w, h, x, y, pixels[index:end].hex()]
        cursor = end
    if cursor != len(pixels):
        raise ValueError("unused bytes in bitmap array")
    return values, (bpp, height, baseline)


def runs(chars):
    for _, group in itertools.groupby(enumerate(chars), lambda pair: pair[1] - pair[0]):
        values = [cp for _, cp in group]
        yield values[0], values[-1]


def seed(folder):
    data = {"schema": 1, "noto_sha256": NOTO_SHA, "previous_assets": OLD_SOURCE, "omitted": sorted(OMIT),
            "faces": {}, "retained_supplement": {}}
    for name in NAMES:
        text = (folder / f"{name}.c").read_text(encoding="utf-8")
        if sha(text.encode("utf-8")) != OLD_SOURCE[name]:
            raise ValueError(f"{name}: old asset SHA-256 differs")
        glyphs, metrics = read_asset(text)
        if metrics != METRICS[name]:
            raise ValueError(f"{name}: old metrics differ")
        data["faces"][name] = {
            "ranges": list(runs(sorted(glyphs))),
            "advance_overrides": {f"{cp:04X}": g[0] for cp, g in sorted(glyphs.items()) if g[0] != 288},
        }
        if name == SUPPLEMENT:
            data["retained_supplement"] = {f"{cp:04X}": glyphs[cp] for cp in sorted(RETAIN)}
    return data


def columns(items, count):
    return "\n".join("    " + ", ".join(items[i:i + count]) + "," for i in range(0, len(items), count))


def codepoints(data, name):
    return [cp for start, end in data["faces"][name]["ranges"] for cp in range(start, end + 1)
            if name != BASE or cp not in OMIT]


def raster(font, cp, advance, metrics):
    from PIL import Image

    bpp, height, baseline = metrics
    mask, (x, y) = font.getmask2(chr(cp), mode="L", anchor="ls")
    bounds = mask.getbbox()
    if bounds is None:
        if cp not in (0x20, 0x3000):
            raise ValueError(f"U+{cp:04X}: printable outline rasterized blank")
        return [advance, 0, 0, 0, 0, ""]
    ink = Image.frombytes("L", mask.size, bytes(mask)).crop(bounds)
    x, y = x + bounds[0], y + bounds[1]
    # Preserve the legacy advances and baseline without cropping overhangs.
    # Horizontal fitting keeps its half-width Latin alphabet and punctuation.
    cell = (advance + 8) // 16
    width, rows = min(ink.width, cell) if cell else ink.width, min(ink.height, height)
    if (width, rows) != ink.size:
        ink = ink.resize((width, rows), Image.Resampling.LANCZOS)
    if cell:
        x = min(max(0, x), cell - width)
    y = min(max(baseline - height, y), baseline - rows)
    alpha = [(v * ((1 << bpp) - 1) + 127) // 255 for v in ink.tobytes()]
    step = 8 // bpp
    alpha.extend([0] * (-len(alpha) % step))
    pixels = bytes(sum(alpha[i + j] << (8 - (j + 1) * bpp) for j in range(step))
                   for i in range(0, len(alpha), step))
    if not any(pixels):
        raise ValueError(f"U+{cp:04X}: ink vanished during quantization")
    return [advance, width, rows, x, -y - rows, pixels.hex()]


def render_c(name, glyphs, metrics):
    bpp, height, baseline = metrics
    points = sorted(glyphs)
    old_marks = "\n".join(" * " + " ".join(f"U+{cp:04X}" for cp in sorted(RETAIN)[i:i+8])
                          for i in range(0, len(RETAIN), 8))
    if name == BASE:
        origin = "/* SPDX-License-Identifier: OFL-1.1\n"
        note = " * U+2252 intentionally absent (no glyph in the pinned font).\n"
    else:
        origin = "/* Mixed font asset. SIL OFL 1.1 applies to regenerated Noto glyphs only.\n"
        note = f""" * The following 34 original glyphs (including their metrics) are byte-exact:
{old_marks}
 * Origin: {OLD_SOURCE['revision']}, old {name}.c SHA-256:
 * {OLD_SOURCE[name]}
 * Old generation input SHA-256: {OLD_SOURCE['supplement_input_sha256']}
 * Their original redistribution status is unchanged; see THIRD_PARTY.md.
"""
    origin += f""" * Noto Sans SC Medium (500), 18 px, {bpp} bpp. Copyright 2014-2021 Adobe.
 * Reserved Font Name: Source. Source SHA-256: {NOTO_SHA}
 * License: {LICENSE}
{note} * Generated by scripts/generate_lcd_fonts.py.
 */"""
    bitmap, rows = bytearray(), ["    {0},"]
    for cp in points:
        advance, w, h, x, y, payload = glyphs[cp]
        vals = (len(bitmap), advance, w, h, x, y)
        rows.append("    {" + ", ".join(f".{key} = {value}" for key, value in zip(FIELDS, vals))
                    + f"}}, /* U+{cp:04X} */")
        bitmap.extend(bytes.fromhex(payload))
    if name == BASE:
        starts = {cp: i for i, cp in enumerate(points, 1)}
        maps = [f"    {{.range_start = {a}, .range_length = {b-a+1}, .glyph_id_start = {starts[a]},\n"
                "     .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}," for a, b in runs(points)]
        lists = ""
        head = "\n#ifndef UI_FONT_CN_18\n#define UI_FONT_CN_18 1\n#endif\n#if UI_FONT_CN_18\n"
        head += "LV_FONT_DECLARE(ui_font_cn_18_supplement);\n"
        head += '#if !LV_FONT_FMT_TXT_LARGE\n#error "UI_FONT_CN_18 requires LV_FONT_FMT_TXT_LARGE"\n#endif\n'
        tail, fallback = "#endif\n", "    .fallback = &ui_font_cn_18_supplement,\n"
        version_fields = "    .underline_position = -2, .underline_thickness = 1,\n"
    else:
        start, end = points[0], points[-1]
        lists = "static const uint16_t unicode_list[] = {\n" + columns([str(i-start) for i in points], 12) + "\n};\n"
        maps = [f"    {{.range_start = {start}, .range_length = {end-start+1}, .glyph_id_start = 1,\n"
                f"     .unicode_list = unicode_list, .list_length = {len(points)},\n"
                "     .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY},"]
        head = tail = fallback = version_fields = ""
    return f"""{origin}
#include "lvgl.h"
{head}
static const LV_ATTRIBUTE_LARGE_CONST uint8_t glyph_bitmap[] = {{
{columns([f'0x{b:02x}' for b in bitmap], 16)}
}};
static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {{
{chr(10).join(rows)}
}};
{lists}static const lv_font_fmt_txt_cmap_t cmaps[] = {{
{chr(10).join(maps)}
}};
#if LVGL_VERSION_MAJOR == 8
static lv_font_fmt_txt_glyph_cache_t cache;
#endif
static const lv_font_fmt_txt_dsc_t font_dsc = {{
    .glyph_bitmap = glyph_bitmap, .glyph_dsc = glyph_dsc, .cmaps = cmaps,
    .cmap_num = {len(maps)}, .bpp = {bpp}, .bitmap_format = LV_FONT_FMT_TXT_PLAIN,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache,
#endif
}};
const lv_font_t {name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {height}, .base_line = {baseline}, .subpx = LV_FONT_SUBPX_NONE,
{version_fields}{fallback}    .dsc = &font_dsc,
}};
{tail}"""


def validate(data, name, text):
    glyphs, metrics = read_asset(text)
    if metrics != METRICS[name] or sorted(glyphs) != codepoints(data, name):
        raise ValueError(f"{name}: metrics or coverage differ")
    face = data["faces"][name]
    for cp, g in glyphs.items():
        key = f"{cp:04X}"
        advance, w, h, x, y, pixels = g
        if advance != face["advance_overrides"].get(key, 288):
            raise ValueError(f"{name}: advance changed U+{key}")
        if name == SUPPLEMENT and cp in RETAIN:
            if data["retained_supplement"][key] != g:
                raise ValueError(f"{name}: original supplemental glyph U+{key} changed")
            continue
        if cp in (0x20, 0x3000) and not pixels:
            continue
        if (not any(bytes.fromhex(pixels)) or x < 0 or x + w > (advance + 8) // 16
                or y < -metrics[2] or y + h > metrics[1] - metrics[2]):
            raise ValueError(f"{name}: blank or out-of-bounds U+{key}")
    if name == BASE and ".fallback = &ui_font_cn_18_supplement" not in text:
        raise ValueError("supplement fallback is missing")
    return glyphs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    task = parser.add_mutually_exclusive_group(required=True)
    task.add_argument("--check", action="store_true")
    task.add_argument("--write", action="store_true")
    task.add_argument("--seed-from", type=Path, help="read original assets only the first time")
    args = parser.parse_args()
    manifest = ASSETS / "font_source.json"
    data = seed(args.seed_from) if args.seed_from else json.loads(manifest.read_text(encoding="utf-8"))
    if (data["schema"] != 1 or data["noto_sha256"] != NOTO_SHA or data["previous_assets"] != OLD_SOURCE
            or data["omitted"] != sorted(OMIT) or {int(cp, 16) for cp in data["retained_supplement"]} != RETAIN):
        raise ValueError("font source manifest does not match the reviewed inputs")
    path = ROOT / NOTO
    if sha(path.read_bytes()) != NOTO_SHA:
        raise ValueError("retained Noto font hash changed")
    if args.check:
        for name in NAMES:
            text = (ASSETS / f"{name}.c").read_text(encoding="ascii")
            if sha(text.encode("ascii")) != data["generated_sha256"][name]:
                raise ValueError(f"{name}: generated asset hash differs")
            glyphs = validate(data, name, text)
            print(f"PASS {name}: {len(glyphs)} glyphs, {sum(len(g[-1]) // 2 for g in glyphs.values())} bitmap bytes")
        return

    import fontTools
    from fontTools.ttLib import TTFont
    from PIL import ImageFont, __version__, features

    with TTFont(path, lazy=True) as sfnt:
        cmap = set(sfnt.getBestCmap())
    if (cmap & (RETAIN | OMIT) or any(cp not in cmap for cp in codepoints(data, BASE))
            or set(codepoints(data, SUPPLEMENT)) - cmap != RETAIN):
        raise ValueError("missing Noto points differ from the exact approved exceptions")
    font = ImageFont.truetype(str(path), 18, layout_engine=ImageFont.Layout.BASIC)
    font.set_variation_by_axes([500])
    results = {}
    for name in NAMES:
        overrides = data["faces"][name]["advance_overrides"]
        for cp in codepoints(data, name):
            if cp in NATURAL and cp not in RETAIN:
                overrides[f"{cp:04X}"] = round(font.getlength(chr(cp)) * 16)
        glyphs = {cp: (data["retained_supplement"][f"{cp:04X}"] if cp in RETAIN
                       else raster(font, cp, overrides.get(f"{cp:04X}", 288), METRICS[name]))
                  for cp in codepoints(data, name)}
        text = render_c(name, glyphs, METRICS[name])
        validate(data, name, text)
        results[name] = text.encode("ascii")
    data["tools"] = {"pillow": __version__, "freetype": features.version_module("freetype2"),
                     "fonttools": fontTools.__version__}
    data["generated_sha256"] = {name: sha(contents) for name, contents in results.items()}
    for name, contents in results.items():
        (ASSETS / f"{name}.c").write_bytes(contents)
    manifest.write_text(json.dumps(data, indent=2) + "\n", encoding="ascii", newline="\n")


if __name__ == "__main__":
    main()
