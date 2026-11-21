"""The source audit must not depend on the Windows ANSI code page.

Replaces legacy: tests/test_structure_encoding.py
"""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from check_structure import audit


class StructureEncoding(unittest.TestCase):
    def test_actual_audit_uses_explicit_text_encoding(self):
        original = Path.read_text

        def strict_read(path, *args, **kwargs):
            self.assertIn(kwargs.get("encoding"), ("utf-8", "utf-8-sig"), str(path))
            return original(path, *args, **kwargs)

        with patch.object(Path, "read_text", strict_read):
            result = audit()
        self.assertTrue(result["static_checks_passed"])
        import product_scope
        self.assertGreaterEqual(result["board_profiles"], product_scope.board_count())
        self.assertIn("6.0/generic-esp32s3", result["sdk_sha256"])
        self.assertIn("5.5/generic-esp32s3", result["sdk_sha256"])


if __name__ == "__main__":
    unittest.main()
