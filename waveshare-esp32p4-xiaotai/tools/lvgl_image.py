"""Encode immutable LVGL images using the P4 byte-swapped RGB565 format."""

from PIL import Image


def encode_rgb565a8(image: Image.Image) -> bytes:
    encoded = bytearray()
    rgba = image.convert("RGBA")
    pixels = rgba.load()
    for y in range(rgba.height):
        for x in range(rgba.width):
            red, green, blue, alpha = pixels[x, y]
            rgb565 = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
            encoded.extend((rgb565 >> 8, rgb565 & 0xFF, alpha))
    return bytes(encoded)


def emit_byte_array(data: bytes) -> list[str]:
    lines: list[str] = []
    for offset in range(0, len(data), 24):
        chunk = data[offset : offset + 24]
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    return lines
