#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline LVGL I1 to companion MONO1 conversion.

This tool is not part of the firmware build. Only emit C after the owner has
authorized the image for redistribution; a successful comparison is not a
license check.
"""
import argparse
from dataclasses import dataclass
from pathlib import Path
import re


PALETTE = bytes((0, 0, 0, 255, 255, 255, 255, 255))
INVERT = bytes(range(255, -1, -1))


@dataclass(frozen=True)
class IndexedImage:
    width: int
    height: int
    stride: int
    data: bytes

    def mono1(self):
        if self.width <= 0 or self.height <= 0 or self.stride != (self.width + 7) // 8:
            raise ValueError("I1 dimensions or stride do not match packed MONO1 rows")
        if len(self.data) != len(PALETTE) + self.height * self.stride:
            raise ValueError("I1 data size does not match its descriptor")
        if self.data[:len(PALETTE)] != PALETTE:
            raise ValueError("I1 palette must have opaque black at index 0 and opaque white at index 1")
        return self.data[len(PALETTE):].translate(INVERT)


def read_legacy_image(path):
    source = path.read_text(encoding="utf-8")
    pixel_array = re.search(r"\bs3_wechat_qr_pixels\s*\[\]\s*=\s*\{([^{}]+)\}\s*;", source)
    descriptor = re.search(r"\.header\s*=\s*\{([^{}]+)\}", source)
    if not pixel_array or not descriptor:
        raise ValueError("expected legacy QR pixel array and LVGL descriptor")
    header = descriptor.group(1)
    if not re.search(r"\.cf\s*=\s*LV_COLOR_FORMAT_I1\b", header):
        raise ValueError("legacy QR descriptor is not LVGL I1")

    def integer(name):
        field = re.search(r"\." + name + r"\s*=\s*(\d+)\s*(?:,|$)", header)
        if not field:
            raise ValueError(f"missing LVGL image field: {name}")
        return int(field.group(1))

    values = pixel_array.group(1).strip().rstrip(",").split(",")
    if not all(re.fullmatch(r"\s*0x[0-9a-fA-F]{2}\s*", value) for value in values):
        raise ValueError("expected a comma-separated list of LVGL image bytes")
    return IndexedImage(integer("w"), integer("h"), integer("stride"),
                        bytes(int(value, 16) for value in values))


def verify_pixels(image, mono):
    """Compare displayed pixels independently of the byte translation."""
    if len(mono) != image.height * image.stride:
        raise ValueError("MONO1 data size does not match its descriptor")
    for y in range(image.height):
        for x in range(image.width):
            byte_offset, shift = divmod(x, 8)
            row_offset = y * image.stride + byte_offset
            mask = 0x80 >> shift
            legacy_index = int(bool(image.data[len(PALETTE) + row_offset] & mask))
            legacy_rgb = image.data[4 * legacy_index:4 * legacy_index + 3]
            mono_ink = bool(mono[row_offset] & mask)
            expected_rgb = b"\x00\x00\x00" if mono_ink else b"\xff\xff\xff"
            if legacy_rgb != expected_rgb:
                raise ValueError(f"pixel differs at ({x}, {y})")
    return image.width * image.height


def render_c(image, mono):
    byte_rows = [
        "    " + ", ".join(f"0x{value:02x}" for value in mono[row:row + 16]) + ","
        for row in range(0, len(mono), 16)
    ]
    return "\n".join((
        "/* Generated companion image; asset licensing is separate from this tool. */",
        '#include "xiaotai/ui.h"',
        "",
        "static const uint8_t wechat_pixels[] = {",
        *byte_rows,
        "};",
        "",
        "const xiaotai_ui_image_t xiaotai_companion_wechat_code = {",
        '    .name = "companion.wechat_code",',
        f"    .width = {image.width},",
        f"    .height = {image.height},",
        "    .format = XIAOTAI_IMAGE_MONO1,",
        "    .data = wechat_pixels,",
        "    .size = sizeof(wechat_pixels),",
        "};",
    ))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="legacy LVGL I1 descriptor to compare")
    parser.add_argument("--emit-authorized-c", action="store_true",
                        help="emit C to stdout only for a build authorized to use the image")
    args = parser.parse_args()
    image = read_legacy_image(args.source)
    mono = image.mono1()
    count = verify_pixels(image, mono)
    if args.emit_authorized_c:
        print(render_c(image, mono))
    else:
        print(f"PASS: {image.width}x{image.height}, stride {image.stride}, {count} visible pixels match")


if __name__ == "__main__":
    main()
