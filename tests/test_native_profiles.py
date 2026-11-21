# Replaces legacy: tests/test_native_profiles.py
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from xiaotai_build import sdk_directory, select, verify_config
import product_scope


class NativeProfiles(unittest.TestCase):
    @unittest.skipUnless(product_scope.has_product(product_scope.XMINI), product_scope.ABSENT)
    def test_mini_archive_is_only_selected_for_xmini(self):
        mini = sdk_directory(select("xmini/c3-v3"))
        generic = sdk_directory(select("m5stack/core-s3"))
        self.assertNotEqual(mini, generic)
        self.assertIn("variants/xmini", mini.as_posix())
        self.assertTrue(generic.as_posix().endswith("v2.6.3__mini/lib"))
        lock = json.loads((mini / "contract.json").read_text())
        self.assertEqual(lock["xmini_contract"]["pending_command_requested_byte_budget"], 16384)

    def test_non_product_wake_model_rejected(self):
        with self.assertRaisesRegex(ValueError, "Non-product wake"):
            verify_config({"CONFIG_SR_WN_WN9_NI3HAO3XIAO3ZHI4_TTS": "y"}, {})

if __name__ == "__main__":
    unittest.main()
