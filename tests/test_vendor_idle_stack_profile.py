"""Generation and isolation checks for the audited Mini idle-stack profile."""

from copy import deepcopy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import sdk_selection as selection  # noqa: E402
import product_scope  # noqa: E402


PROFILE = "TIRTC_IDLE_STACK_COMPACT"
PIN = f"CONFIG_XIAOTAI_{PROFILE}"
SESSION = "CONFIG_XIAOTAI_TIRTC_SESSION_STACK_9216=y"
XMINI = "xiaotai-xmini-c3-v3"
COLOR = ("xiaotai-cores3-lite", "xiaotai-szpi-s3", "xiaotai-waveshare-p4-rev3", "xiaotai-wt99p461-s1-1v1")


def product(name=XMINI):
    return selection.load_product(ROOT / "products" / name / "product.json")


def options(value, state):
    return dict(value, options={**value.get("options", {}), PROFILE: state})


class VendorIdleStackProfile(unittest.TestCase):
    def test_four_external_ram_products_unchanged_with_default_off(self):
        with tempfile.TemporaryDirectory() as folder:
            scratch = Path(folder)
            for name in COLOR:
                with self.subTest(product=name):
                    item = product(name)
                    normal, off = scratch / name, scratch / (name + "-explicit-off")
                    a = selection.write(ROOT, item, normal)
                    b = selection.write(ROOT, options(item, False), off)
                    self.assertNotIn(PIN, (normal / "sdkconfig.selection").read_text(encoding="utf-8"))
                    self.assertIn(PIN + "=n", b.pop("options"))
                    self.assertEqual(a["selected"], b["selected"])
                    files = ("xiaotai_selected.c", "xiaotai_product.h", "selection.cmake", "commands.json")
                    for relative in files:
                        self.assertEqual((normal / relative).read_bytes(), (off / relative).read_bytes())
                    with self.assertRaisesRegex(selection.SelectionError, "without external RAM"):
                        selection.plan(ROOT, options(item, True))

    @unittest.skipUnless(product_scope.has_product(product_scope.XMINI), product_scope.ABSENT)
    def test_xmini_default_is_compact_and_profile_changes_only_one_option(self):
        with tempfile.TemporaryDirectory() as folder:
            scratch = Path(folder)
            item = product()
            original = deepcopy(item)
            self.assertIs(item["options"][PROFILE], True)
            normal, compact = scratch / "uniform", scratch / "default"
            a = selection.write(ROOT, options(item, False), normal)
            b = selection.write(ROOT, item, compact)
            self.assertEqual(item, original)
            self.assertIn(PIN + "=y", b["options"])
            self.assertEqual(a["options"], [PIN + "=n" if line == PIN + "=y" else line for line in b["options"]])
            self.assertIn(SESSION, b["options"])
            self.assertEqual(a["selected"], b["selected"])
            self.assertEqual(a["tirtc"], b["tirtc"])
            self.assertEqual((normal / "xiaotai_selected.c").read_bytes(), (compact / "xiaotai_selected.c").read_bytes())
            self.assertIn(PIN + "=y", selection.plan(ROOT, selection.apply_variant(item, "diagnostics"))["options"])

    @unittest.skipUnless(product_scope.has_product(product_scope.XMINI), product_scope.ABSENT)
    def test_wrong_chip_memory_or_package_is_rejected(self):
        item = options(product(), True)
        board = json.loads((ROOT / "boards" / item["board"] / "board.json").read_text(encoding="utf-8"))
        native = selection.plan(ROOT, item)
        cases = (
            (item, dict(board, target="esp32s3")),
            (item, dict(board, memory={"external_ram": True})),
            (item, dict(board, memory={})),
            (item, dict(board, memory={"external_ram": 0})),
            (dict(item, tirtc={"6.0": "generic"}), board),
            (dict(item, tirtc={"6.0": "xmini-mini", "5.5": "generic"}), board),
        )
        for selection_input, board_input in cases:
            with self.subTest(target=board_input.get("target"), memory=board_input.get("memory"),
                              tirtc=selection_input["tirtc"]), self.assertRaisesRegex(selection.SelectionError, PROFILE):
                selection._options(ROOT, selection_input, native["components"], board=board_input)

    @unittest.skipUnless(product_scope.has_product(product_scope.XMINI), product_scope.ABSENT)
    def test_cannot_mix_profile_and_uniform_override(self):
        for size in (0, 2048, 2560, 4096.0, "4096", 6144, 8192, None):
            item = options(product(), True)
            item["parameters"]["transport"]["vendor_idle_stack_bytes"] = size
            with self.subTest(size=size), self.assertRaisesRegex(selection.SelectionError, "vendor_idle_stack_bytes=4096"):
                selection.plan(ROOT, item)
        item = product()
        item["parameters"]["transport"]["vendor_idle_stack_bytes"] = 0
        apps, units, board = selection.plan(ROOT, options(item, False))["units"]
        self.assertIn(".vendor_idle_stack_bytes = 0", selection.generate(apps, units, item, board))

    @unittest.skipUnless(product_scope.has_product(product_scope.XMINI), product_scope.ABSENT)
    def test_requires_explicit_boolean(self):
        for bad in ("true", "", 0, 1):
            with self.subTest(bad=bad), self.assertRaisesRegex(selection.SelectionError, "expected a boolean"):
                selection.plan(ROOT, options(product(), bad))


if __name__ == "__main__":
    unittest.main()
