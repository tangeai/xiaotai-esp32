"""Verify the Xmini OLED snapshot format without a serial connection."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from decode_screen import decode


HEADER = b"+SCREEN: width=128 height=64 format=SSD1306_PAGE bytes=1024 source=last_sent\r\n"


class XminiScreenTests(unittest.TestCase):
    def setUp(self):
        pixels = bytearray(1024)
        pixels[0] = 0x81
        pixels[128] = 0x01
        pixels[1023] = 0x80
        chunks = [f"+SCREEN: offset={offset} hex={pixels[offset:offset+32].hex()}\r\n".encode()
                  for offset in range(0, 1024, 32)]
        self.frame = HEADER + b"I (42) unrelated log\r\n" + b"".join(chunks) + b"OK\r\n"

    def test_page_layout_and_bit_order(self):
        width, height, rgb = decode(self.frame)
        self.assertEqual((width, height), (128, 64))
        self.assertEqual(len(rgb), 128 * 64 * 3)
        lit = {index for index in range(width * height) if rgb[index * 3:index * 3 + 3] == b"\xff" * 3}
        self.assertEqual(lit, {0, 7 * 128, 8 * 128, 64 * 128 - 1})

    def test_rejects_incomplete_or_ambiguous_transfers(self):
        cases = (
            self.frame.replace(b"height=64", b"height=32"),
            self.frame.replace(b"source=last_sent", b"source=generated"),
            self.frame.replace(b"offset=32 ", b"offset=0 "),
            self.frame.replace(b"offset=32 hex=00", b"offset=32 hex="),
            self.frame.replace(b"offset=64 ", b"offset=65 "),
            self.frame.replace(b"OK\r\n", b"ERROR: ESP_FAIL\r\n"),
            self.frame.removesuffix(b"OK\r\n"),
            self.frame.removesuffix(b"\r\n"),
            self.frame + self.frame,
        )
        for index, bad in enumerate(cases):
            with self.subTest(case=index), self.assertRaises(ValueError):
                decode(bad)


if __name__ == "__main__":
    unittest.main()
