"""Validate a local AT+SCREEN transfer and write a PNG (no serial/device action)."""
import argparse
from pathlib import Path
import re
import struct
import zlib


def decode(log):
    header = re.search(rb"\+SCREEN: format=RGB565LE width=(\d+) height=(\d+) bytes=(\d+) fnv=([0-9a-f]{8})\r?\n", log)
    if not header:
        raise ValueError("No complete RGB565LE frame header")
    width, height, size = map(int, header.group(1, 2, 3))
    expected_hash = int(header[4], 16)
    if not 0 < width <= 1024 or not 0 < height <= 1024 or size != width * height * 2:
        raise ValueError("Invalid frame dimensions/size")
    end = re.search(rb"\+SCREEN-END: fnv=([0-9a-f]{8})\r?\n", log[header.end():])
    if not end or int(end[1], 16) != expected_hash:
        raise ValueError("Missing/mismatched frame trailer")
    data = bytearray()
    records = re.findall(rb"\+SCREEN-DATA: seq=(\d+) hex=([0-9a-f]+)\r?\n", log[header.end():header.end() + end.start()])
    for expected, (seq, payload) in enumerate(records):
        if int(seq) != expected or len(payload) % 2 or len(payload) != min(128, size - len(data)) * 2:
            raise ValueError("Missing, repeated or malformed frame chunk")
        data.extend(bytes.fromhex(payload.decode("ascii")))
    if len(data) != size:
        raise ValueError("Incomplete frame")
    digest = 2166136261
    for byte in data:
        digest = ((digest ^ byte) * 16777619) & 0xffffffff
    if digest != expected_hash:
        raise ValueError("Frame checksum mismatch")
    rgb = bytearray()
    for (pixel,) in struct.iter_unpack("<H", data):
        rgb.extend((((pixel >> 11) & 31) * 255 // 31, ((pixel >> 5) & 63) * 255 // 63, (pixel & 31) * 255 // 31))
    return width, height, bytes(rgb)


def png(width, height, rgb):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    width, height, rgb = decode(args.log.read_bytes())
    with args.output.open("xb") as stream:
        stream.write(png(width, height, rgb))
    print(f"Verified {width}x{height} render; {len(set(struct.iter_unpack('BBB', rgb)))} distinct colors. Not LCD readback.")
