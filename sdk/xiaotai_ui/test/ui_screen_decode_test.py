"""Offline fixtures for the unified SDK AT+SCREEN blob."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from decode_screen import decode  # noqa: E402


def blob(pixel, width, height, data):
    checksum = 2166136261
    for byte in data:
        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
    rows = [f"+SCREEN: format={pixel} width={width} height={height} view=home%20clock "
            f"bytes={len(data)} fnv={checksum:08x}"]
    chunks = [data[i:i + 128] for i in range(0, len(data), 128)]
    rows.extend(f"+SCREEN-DATA: seq={i} hex={chunk.hex().upper()}" for i, chunk in enumerate(chunks))
    rows.extend([f"+SCREEN-END: chunks={len(chunks)} fnv={checksum:08x}", "OK", ""])
    return "\r\n".join(rows).encode("ascii")


class ScreenDecode(unittest.TestCase):
    def test_formats(self):
        self.assertEqual(decode(blob("RGB565LE", 3, 1, b"\x00\xf8\xe0\x07\x1f\x00")),
                         (3, 1, b"\xff\0\0\0\xff\0\0\0\xff"))
        self.assertEqual(decode(blob("MONO1_VLSB", 2, 2, b"\x02\x01")),
                         (2, 2, b"\0\0\0" + b"\xff" * 6 + b"\0\0\0"))
        self.assertEqual(decode(blob("MONO1", 9, 1, b"\x80\x80")),
                         (9, 1, b"\xff" * 3 + b"\0" * 21 + b"\xff" * 3))

    def test_damaged_transfer(self):
        data = bytes(range(130))
        valid = blob("RGB565LE", 65, 1, data)
        for broken in [valid.replace(b"seq=1", b"seq=0"), valid.replace(b"chunks=2", b"chunks=1"),
                       valid.replace(b"hex=000102", b"hex=ff0102"), valid.replace(b"bytes=130", b"bytes=129"),
                       valid.replace(b"+SCREEN-END: chunks=2", b"+SCREEN-END:"),
                       valid[:valid.index(b"\r\nOK")], valid + valid]:
            with self.subTest(failure=broken[:70]), self.assertRaises(ValueError):
                decode(broken)

    def test_old_little_endian(self):
        old = blob("RGB565LE", 1, 1, b"\xe0\x07")
        old = old.replace(b" view=home%20clock", b"").replace(b"chunks=1 ", b"")
        self.assertEqual(decode(old), (1, 1, b"\0\xff\0"))

    def test_preview_bounds_follow_pixel_budget(self):
        wide = blob("MONO1", 1280, 1, bytes(160))
        self.assertEqual(decode(wide), (1280, 1, bytes(1280 * 3)))
        with self.assertRaises(ValueError):
            decode(wide.replace(b"width=1280 height=1", b"width=4096 height=4096"))


if __name__ == "__main__":
    unittest.main()
