"""Exercise the SDK screen transfer and PNG output without a device."""
# Replaces legacy: tests/test_xmini_screen_decode.py
# Replaces legacy: tests/test_at_diagnostics.py
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from decode_screen import decode, decode_lines, png


def wire_frame(pixel_format, width, height, raw):
    digest = 0x811C9DC5
    for octet in raw:
        digest = ((digest ^ octet) * 0x01000193) & 0xFFFFFFFF
    chunks = [raw[offset:offset + 128] for offset in range(0, len(raw), 128)]
    records = [f"+SCREEN: format={pixel_format} width={width} height={height} view=home bytes={len(raw)} fnv={digest:08x}"]
    records.extend(f"+SCREEN-DATA: seq={i} hex={chunk.hex().upper()}" for i, chunk in enumerate(chunks))
    records.extend((f"+SCREEN-END: chunks={len(chunks)} fnv={digest:08x}", "OK"))
    return ("\r\n".join(records) + "\r\n").encode("ascii")


class SdkScreenTests(unittest.TestCase):
    def setUp(self):
        # 130 bytes: one full record and then a one-pixel tail.
        raw = bytes.fromhex("00F8E0071F00") + bytes(122) + bytes.fromhex("FFFF")
        self.frame = wire_frame("RGB565LE", 13, 5, raw)

    def test_rgb565le_pixels_and_png_dimensions(self):
        noisy = self.frame.replace(b"+SCREEN-DATA: seq=1", b"I (91) unrelated output\r\n+SCREEN-DATA: seq=1")
        width, height, rgb = decode(noisy, sdk_only=True)
        self.assertEqual((width, height), (13, 5))
        self.assertEqual(rgb, bytes.fromhex("FF000000FF000000FF") + bytes(183) + b"\xff" * 3)
        self.assertEqual(decode(noisy), (width, height, rgb))
        self.assertEqual(decode_lines(noisy.decode("ascii").splitlines(), sdk_only=True), (width, height, rgb))
        encoded = png(width, height, rgb)
        self.assertTrue(encoded.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(struct.unpack(">II", encoded[16:24]), (13, 5))

    def test_mono_formats_and_padding(self):
        cases = (
            ("MONO1", 9, 2, bytes.fromhex("80804000"), {(0, 0), (8, 0), (1, 1)}),
            ("MONO1_VLSB", 3, 9, bytes.fromhex("810002000100"), {(0, 0), (0, 7), (2, 1), (1, 8)}),
        )
        for fmt, width, height, raw, expected in cases:
            with self.subTest(format=fmt):
                w, h, rgb = decode(wire_frame(fmt, width, height, raw), sdk_only=True)
                self.assertEqual((w, h), (width, height))
                self.assertEqual(len(rgb), w * h * 3)
                self.assertEqual({(i % w, i // w) for i in range(w * h) if rgb[i * 3] == 255}, expected)

    def test_rejects_incomplete_or_corrupt_sdk_reply(self):
        broken = {
            "missing chunk": self.frame.replace(b"+SCREEN-DATA: seq=1 hex=FFFF\r\n", b""),
            "repeated sequence": self.frame.replace(b"seq=1", b"seq=0"),
            "wrong length for dimensions": self.frame.replace(b"width=13", b"width=14"),
            "oversize preview": self.frame.replace(b"width=13 height=5", b"width=4096 height=4096"),
            "bad data checksum": self.frame.replace(b"00F8", b"00F9"),
            "missing end count": self.frame.replace(b"chunks=2 ", b""),
            "wrong end count": self.frame.replace(b"chunks=2", b"chunks=3"),
            "premature OK": self.frame.replace(b"+SCREEN-DATA: seq=1", b"OK\r\n+SCREEN-DATA: seq=1"),
            "missing OK": self.frame.removesuffix(b"OK\r\n"),
            "partial OK": self.frame.removesuffix(b"\r\n"),
            "failed after trailer": self.frame.replace(b"OK\r\n", b"ERROR: TIMEOUT STREAM\r\n"),
            "multiple frames": self.frame + self.frame,
        }
        for name, data in broken.items():
            with self.subTest(case=name), self.assertRaises(ValueError):
                decode(data, sdk_only=True)

    def test_legacy_s3_no_view_or_chunk_count(self):
        legacy = self.frame.replace(b" view=home", b"").replace(b"chunks=2 ", b"").removesuffix(b"OK\r\n")
        self.assertEqual(decode(legacy), decode(self.frame, sdk_only=True))
        with self.assertRaisesRegex(ValueError, "view"):
            decode(legacy, sdk_only=True)

    def test_xmini_page_geometry_and_strict_framing_in_both_dialects(self):
        raw = bytearray(1024)
        raw[0], raw[128], raw[1023] = 0x81, 0x01, 0x80
        legacy = (b"+SCREEN: width=128 height=64 format=SSD1306_PAGE bytes=1024 source=last_sent\r\n"
                  b"I (42) unrelated log\r\n" + b"".join(
                      f"+SCREEN: offset={offset} hex={raw[offset:offset + 32].hex()}\r\n".encode("ascii")
                      for offset in range(0, len(raw), 32)) + b"OK\r\n")
        sdk = wire_frame("MONO1_VLSB", 128, 64, raw)
        width, height, rgb = decode(legacy)
        self.assertEqual((width, height, len(rgb)), (128, 64, 128 * 64 * 3))
        lit = {i for i in range(width * height) if rgb[3 * i:3 * i + 3] == b"\xff" * 3}
        self.assertEqual(lit, {0, 7 * 128, 8 * 128, 64 * 128 - 1})
        self.assertEqual(decode(sdk, sdk_only=True), (width, height, rgb))
        with self.assertRaises(ValueError):
            decode(legacy, sdk_only=True)

        for dialect, frame, mutations in (
            ("legacy", legacy, ((b"source=last_sent", b"source=generated"), (b"offset=32", b"offset=0"),
                                 (b"offset=32 hex=00", b"offset=32 hex="), (b"offset=64", b"offset=65"))),
            ("sdk", sdk, ((b"view=home", b"view=home source=generated"), (b"seq=1", b"seq=0"),
                           (b"seq=1 hex=01", b"seq=1 hex="), (b"seq=2", b"seq=3"))),
        ):
            corrupt = [frame.replace(b"height=64", b"height=32", 1),
                       frame.replace(b"OK\r\n", b"ERROR: ESP_FAIL\r\n"),
                       frame.removesuffix(b"OK\r\n"), frame.removesuffix(b"\r\n"), frame + frame]
            for old, new in mutations:
                self.assertIn(old, frame)
                corrupt.append(frame.replace(old, new, 1))
            for i, bad in enumerate(corrupt):
                with self.subTest(dialect=dialect, case=i), self.assertRaises(ValueError):
                    decode(bad, sdk_only=dialect == "sdk")


if __name__ == "__main__":
    unittest.main()
