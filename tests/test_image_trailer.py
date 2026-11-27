"""Data behind an application image: where it lands, that attaching again replaces it, and what verification refuses."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

import image_trailer  # noqa: E402


def image(segments, hash_appended=True):
    """An application image as the loader reads it: header, segments, padding to a checksum, a hash."""
    data = bytearray(24)
    data[0], data[1], data[23] = 0xE9, len(segments), 1 if hash_appended else 0
    for body in segments:
        data += struct.pack("<II", 0x40000000, len(body)) + body
    data += bytes(15 - len(data) % 16) + b"\x5a"
    if hash_appended:
        data += bytes(range(32))
    return bytes(data)


class ImageTrailerTests(unittest.TestCase):
    def test_length_follows_the_headers(self):
        for sizes, hashed in (((5,), True), ((16, 3), True), ((7, 0x12345), False), ((), True)):
            data = image([bytes(n) for n in sizes], hashed)
            self.assertEqual(image_trailer.image_length(data), len(data))
            self.assertEqual(image_trailer.image_length(data + b"anything after it"), len(data))

    def test_data_lands_on_the_next_page(self):
        app, data = image([bytes(0x12345)]), bytes(range(256)) * 20
        out = image_trailer.attached(app, data)
        self.assertEqual(out[:len(app)], app)
        self.assertEqual(set(out[len(app):0x20000]), {0xFF})
        self.assertEqual(out[0x20000:], data)
        # An image that ends exactly on a page leaves no gap.
        exact = image([bytes(0x10000 - 24 - 8 - 16 - 32)])
        self.assertEqual(len(exact), 0x10000)
        self.assertEqual(image_trailer.attached(exact, b"x"), exact + b"x")

    def test_attaching_again_replaces(self):
        app = image([bytes(100)])
        once = image_trailer.attached(app, b"first" * 100)
        self.assertEqual(image_trailer.attached(once, b"first" * 100), once)
        self.assertEqual(image_trailer.attached(once, b"second"), image_trailer.attached(app, b"second"))

    def test_verification(self):
        app = image([bytes(5000)])
        out = image_trailer.attached(app, b"pictures" * 1000)
        record = image_trailer.describe(out, "character.pack")
        self.assertEqual((record["offset"], record["bytes"], record["image_bytes"]), ("0x10000", 8000, len(app)))
        image_trailer.verify(out, record)
        image_trailer.verify(app, None)
        refused = (
            (out, None),                                        # data nobody recorded
            (app, record),                                      # recorded data that is not there
            (out[:-1], record), (out + b"\0", record),          # cut short, something more
            (out[:-1] + b"?", record),                          # other data
            (out[:len(app)] + b"\0" + out[len(app) + 1:], record),      # a gap that is not blank
            (out, {**record, "offset": "0x20000"}), (out, {**record, "sha256": "0" * 64}),
            (app, {**record, "bytes": 0}))
        for data, against in refused:
            with self.assertRaises(ValueError):
                image_trailer.verify(data, against)

    def test_refuses_what_is_not_an_image(self):
        for data in (b"", b"not an image, whatever it is, at all", image([bytes(64)])[:40], image([bytes(64)])[:-1]):
            with self.assertRaises(ValueError):
                image_trailer.attached(data, b"data")
        with self.assertRaises(ValueError):
            image_trailer.attached(image([bytes(8)]), b"")

    def test_command_records_what_it_attached(self):
        with tempfile.TemporaryDirectory() as work:
            app, data, record = (Path(work) / name for name in ("app.bin", "some.pack", "image_trailer.json"))
            app.write_bytes(image([bytes(700)]))
            data.write_bytes(b"pictures")
            for _ in range(2):
                image_trailer.main(["attach", str(app), str(data), "--record", str(record)])
                image_trailer.verify(app.read_bytes(), json.loads(record.read_text(encoding="utf-8")))
            self.assertEqual(json.loads(record.read_text(encoding="utf-8"))["source"], "some.pack")


if __name__ == "__main__":
    unittest.main()
