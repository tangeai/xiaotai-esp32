"""Validate a local AT+SCREEN transfer and write a PNG (no serial/device action)."""
import argparse
from pathlib import Path
import re
import struct
import zlib

XMINI_FORMAT = re.compile(rb"(?m)^\+SCREEN: [^\r\n]*format=SSD1306_PAGE(?: |\r?$)")
XMINI_HEADER = b"+SCREEN: width=128 height=64 format=SSD1306_PAGE bytes=1024 source=last_sent"
XMINI_CHUNK = re.compile(rb"\+SCREEN: offset=(\d+) hex=([0-9a-f]{64})")
BLOB_HEADER = re.compile(
    rb"\+SCREEN: format=(RGB565LE|MONO1_VLSB|MONO1) width=(\d{1,5}) height=(\d{1,5})"
    rb"(?: view=([!-~]{1,94}))? bytes=(\d{1,7}) fnv=([0-9a-fA-F]{8})"
)
BLOB_DATA = re.compile(rb"\+SCREEN-DATA: seq=(\d{1,5}) hex=([0-9a-fA-F]{2,256})")
BLOB_END = re.compile(rb"\+SCREEN-END: (?:chunks=(\d{1,5}) )?fnv=([0-9a-fA-F]{8})")
BLOB_CHUNK_BYTES = 128
MAX_PREVIEW_PIXELS = 4_194_304


def decode_rgb(data, byte_order):
    rgb = bytearray()
    for (pixel,) in struct.iter_unpack(byte_order + "H", data):
        rgb.extend((((pixel >> 11) & 31) * 255 // 31, ((pixel >> 5) & 63) * 255 // 63, (pixel & 31) * 255 // 31))
    return bytes(rgb)


def decode_cores3(log):
    header = re.compile(rb"\+SCREEN:320,240,rgb565-swap,page=\d+,bytes=153600,crc=([0-9a-f]{8})")
    chunk = re.compile(rb"\+PIX:(\d+):([0-9a-f]{128})")
    data = bytearray()
    digest = None
    complete = False
    for line in log.splitlines():
        if line == b"+SCREEN:END":
            if digest is None or complete or len(data) != 153600:
                raise ValueError("Unexpected CoreS3 frame trailer")
            complete = True
        elif line.startswith(b"+SCREEN:"):
            match = header.fullmatch(line)
            if digest is not None or not match:
                raise ValueError("Unexpected CoreS3 frame header")
            digest = int(match[1], 16)
        elif line.startswith(b"+PIX:"):
            match = chunk.fullmatch(line)
            if digest is None or complete or not match or len(data) >= 153600 or int(match[1]) != len(data):
                raise ValueError("Missing, repeated or malformed CoreS3 frame chunk")
            data.extend(bytes.fromhex(match[2].decode("ascii")))
    if not complete or zlib.crc32(data) != digest:
        raise ValueError("Incomplete CoreS3 frame or CRC mismatch")
    return 320, 240, decode_rgb(data, ">")


def decode_xmini(log):
    """Decode one last-sent OLED snapshot; the wire format has no checksum."""
    width, height = 128, 64
    size = width * height // 8
    data = bytearray()
    started = complete = False
    for raw in log.splitlines(keepends=True):
        line = raw.rstrip(b"\r\n")
        if line.startswith(b"+SCREEN:"):
            if complete or not raw.endswith(b"\n"):
                raise ValueError("Unexpected or truncated Xmini screen record")
            if not started:
                if line != XMINI_HEADER:
                    raise ValueError("Invalid Xmini frame header")
                started = True
                continue
            match = XMINI_CHUNK.fullmatch(line)
            if not match or len(data) >= size or int(match[1]) != len(data):
                raise ValueError("Missing, repeated or malformed Xmini frame chunk")
            data.extend(bytes.fromhex(match[2].decode("ascii")))
        elif started and not complete:
            if line == b"OK":
                if not raw.endswith(b"\n") or len(data) != size:
                    raise ValueError("Incomplete Xmini screen response")
                complete = True
            elif line == b"ERROR" or line.startswith((b"ERROR:", b"ERROR,")):
                raise ValueError("Xmini screen command failed")
    if not complete:
        raise ValueError("Missing Xmini screen OK")
    return width, height, decode_mono(data, width, height, True)


def decode_mono(data, width, height, vertical):
    rgb = bytearray()
    stride = (width + 7) // 8
    for y in range(height):
        for x in range(width):
            if vertical:
                pixel = data[y // 8 * width + x] & (1 << (y % 8))
            else:
                pixel = data[y * stride + x // 8] & (0x80 >> (x % 8))
            rgb.extend(b"\xff\xff\xff" if pixel else b"\0\0\0")
    return bytes(rgb)


def decode_blob(log, *, sdk_only=False):
    frame = None
    data = bytearray()
    chunks = 0
    completed = acknowledged = sdk = False
    for raw in log.splitlines(keepends=True):
        line = raw.rstrip(b"\r\n")
        if not line.startswith((b"+SCREEN:", b"+SCREEN-DATA:", b"+SCREEN-END:")):
            if sdk and not acknowledged:
                if line == b"OK":
                    if not completed or not raw.endswith(b"\n"):
                        raise ValueError("Incomplete SDK screen response")
                    acknowledged = True
                elif line == b"ERROR" or line.startswith(b"ERROR:"):
                    raise ValueError("SDK screen command failed")
            continue
        if completed or not raw.endswith(b"\n"):
            raise ValueError("Unexpected or truncated frame record")
        if line.startswith(b"+SCREEN:"):
            match = BLOB_HEADER.fullmatch(line)
            if frame is not None or not match:
                raise ValueError("Invalid frame header")
            pixel, w, h, view, total, fnv = match.groups()
            sdk = view is not None
            if (sdk_only or pixel != b"RGB565LE") and not sdk:
                raise ValueError("Missing SDK screen view")
            width, height, size = map(int, (w, h, total))
            limit = 65535 if sdk else 1024
            expected_hash = int(fnv, 16)
            expected = (width * height * 2 if pixel == b"RGB565LE" else
                        width * ((height + 7) // 8) if pixel == b"MONO1_VLSB" else
                        height * ((width + 7) // 8))
            if (not 0 < width <= limit or not 0 < height <= limit or
                    width * height > MAX_PREVIEW_PIXELS or size != expected):
                raise ValueError("Invalid frame dimensions/size")
            frame = (pixel, width, height, size, expected_hash)
        elif frame is None:
            raise ValueError("Frame data before its header")
        elif line.startswith(b"+SCREEN-DATA:"):
            match = BLOB_DATA.fullmatch(line)
            needed = min(BLOB_CHUNK_BYTES, frame[3] - len(data))
            if not match or needed <= 0 or int(match[1]) != chunks or len(match[2]) != needed * 2:
                raise ValueError("Missing, repeated or malformed frame chunk")
            data.extend(bytes.fromhex(match[2].decode("ascii")))
            chunks += 1
        else:
            match = BLOB_END.fullmatch(line)
            if (not match or int(match[2], 16) != frame[4] or len(data) != frame[3] or
                    sdk != (match[1] is not None) or
                    (match[1] is not None and int(match[1]) != chunks)):
                raise ValueError("Missing/mismatched frame trailer")
            completed = True
    if not completed:
        raise ValueError("Incomplete frame")
    if sdk and not acknowledged:
        raise ValueError("Missing SDK screen OK")
    pixel, width, height, _, expected_hash = frame
    digest = 2166136261
    for byte in data:
        digest = ((digest ^ byte) * 16777619) & 0xffffffff
    if digest != expected_hash:
        raise ValueError("Frame checksum mismatch")
    rgb = decode_rgb(data, "<") if pixel == b"RGB565LE" else decode_mono(data, width, height, pixel == b"MONO1_VLSB")
    return width, height, rgb


def decode(log, *, sdk_only=False):
    """Return (width, height, RGB bytes) for one screenshot from a wire log."""
    if sdk_only:
        return decode_blob(log, sdk_only=True)
    if XMINI_FORMAT.search(log):
        return decode_xmini(log)
    if re.search(rb"(?m)^\+SCREEN:\d+,\d+,rgb565-swap,", log):
        return decode_cores3(log)
    return decode_blob(log)


def decode_lines(lines, *, sdk_only=False):
    """Decode complete, newline-stripped lines from the bench console reader."""
    return decode(("\n".join(lines) + "\n").encode("utf-8"), sdk_only=sdk_only)


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
    parser.add_argument("--sdk-only", action="store_true", help="require an acknowledged unified SDK frame")
    args = parser.parse_args()
    log = args.log.read_bytes()
    try:
        width, height, rgb = decode(log, sdk_only=args.sdk_only)
    except ValueError as error:
        parser.error(str(error))
    with args.output.open("xb") as stream:
        stream.write(png(width, height, rgb))
    colors = len(set(struct.iter_unpack("BBB", rgb)))
    if XMINI_FORMAT.search(log):
        print(f"Verified {width}x{height} Xmini frame structure; {colors} colors. No wire checksum or panel readback.")
    else:
        print(f"Verified {width}x{height} render; {colors} distinct colors. Not LCD readback.")
