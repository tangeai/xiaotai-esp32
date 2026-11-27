#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify the conversion and the exact legacy image shipped by the companion."""
from hashlib import sha256
from pathlib import Path
import sys
import unittest

from wechat_code_image import IndexedImage, read_legacy_image, render_c, verify_pixels


APP = Path(__file__).resolve().parents[1]
ROOT = APP.parents[1]


class WechatCodeTest(unittest.TestCase):
    def setUp(self):
        self.palette = bytes((0, 0, 0, 255, 255, 255, 255, 255))
        self.pixels = bytes((0x80, 0x7F, 0x01, 0x80))
        self.image = IndexedImage(9, 2, 2, self.palette + self.pixels)

    def test_palette_inversion_and_partial_byte_per_row(self):
        mono = self.image.mono1()
        self.assertEqual(mono, bytes((0x7F, 0x80, 0xFE, 0x7F)))
        self.assertEqual(verify_pixels(self.image, mono), 18)
        c = render_c(self.image, mono)
        self.assertIn('const xiaotai_ui_image_t xiaotai_companion_wechat_code = {', c)
        self.assertIn('.name = "companion.wechat_code",', c)
        self.assertIn('.format = XIAOTAI_IMAGE_MONO1,', c)
        self.assertIn('0x7f, 0x80, 0xfe, 0x7f,', c)

    def test_comparison_detects_wrong_pixel_in_partial_byte(self):
        wrong = bytearray(self.image.mono1())
        wrong[3] ^= 0x80
        with self.assertRaisesRegex(ValueError, r"pixel differs at \(8, 1\)"):
            verify_pixels(self.image, wrong)

    def test_refuses_wrong_layout_or_palette(self):
        data = self.image.data
        invalid = (
            (IndexedImage(9, 2, 4, data), "stride"),
            (IndexedImage(9, 2, 2, data[:-1]), "size"),
            (IndexedImage(9, 2, 2, data[4:8] + data[:4] + data[8:]), "palette"),
        )
        for image, error in invalid:
            with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                image.mono1()

    def test_original_and_shipped_bytes(self):
        canonical = (APP / "assets/wechat_code.c").read_text(encoding="utf-8")
        # The legacy S3, P4 and WT99 UIs shipped byte-identical copies of this image.
        for source in (APP / "test/legacy_wechat_qr.inc",):
            with self.subTest(source=source.name):
                self.assertEqual(sha256(source.read_bytes()).hexdigest(),
                                 "fd3e69e498d8df6546b73f264e6eaae62fd0657310439de4d468470f37d0d9f1")
                image = read_legacy_image(source)
                mono = image.mono1()
                self.assertEqual((image.width, image.height, image.stride), (185, 185, 24))
                self.assertEqual(sha256(mono).hexdigest(),
                                 "3adea0dddb741ea43cff3188c8045f95209f14ddc37201c8992adb1a923a1e66")
                self.assertEqual(verify_pixels(image, mono), 34225)
                self.assertEqual(canonical, render_c(image, mono) + "\n")

    def test_existing_product_options_and_variants(self):
        fixtures = ROOT / "sdk/themes/xiaotai_theme_lcd_kit/test"
        if not (fixtures / "kit_selection_test.py").is_file() or not (ROOT / "products").is_dir():
            self.skipTest("product selection fixtures are not in this SDK package")
        sys.path.insert(0, str(fixtures))
        try:
            from kit_selection_test import PRODUCTS, load_selection
        finally:
            sys.path.pop(0)
        selection = load_selection(ROOT)
        for name, (_, _, themes) in PRODUCTS.items():
            manifest = ROOT / "products" / name / "product.json"
            if not manifest.is_file():
                continue  # a source release may leave out an optional product
            product = selection.load_product(manifest)
            for variant in (None, *product.get("variants", {})):
                with self.subTest(product=name, variant=variant):
                    choice = selection.apply_variant(product, variant) if variant else product
                    plan = selection.plan(ROOT, choice)
                    enabled = "CONFIG_XIAOTAI_COMPANION_WECHAT_CODE=y" in plan["options"]
                    self.assertEqual(enabled, "theme_lcd_kit" in themes)
                    if enabled:
                        self.assertIn("xiaotai_ui", plan["components"])
                        self.assertIn("CONFIG_XIAOTAI_UI_LVGL=y", plan["options"])


if __name__ == "__main__":
    unittest.main()
