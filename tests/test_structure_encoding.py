"""The source audit must not depend on the Windows ANSI code page."""
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
        self.assertGreaterEqual(result["board_profiles"], 6)
        self.assertIn("szpi-http", result["sdk_sha256"])


if __name__ == "__main__":
    unittest.main()
