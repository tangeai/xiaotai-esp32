"""Keep the checked-in AT table tied to the real product plans."""
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from generate_at_reference import END, MIGRATION, START, check_migration, usage_forms, vocabulary_table
from sdk_selection import SelectionError, catalog, vocabulary


class AtReference(unittest.TestCase):
    def test_checked_in_table_matches_manifest(self):
        document = (ROOT / "docs/AT指令.md").read_text(encoding="utf-8")
        self.assertEqual(document.count(START), 1)
        self.assertEqual(document.count(END), 1)
        actual = document.split(START, 1)[1].split(END, 1)[0]
        self.assertEqual(actual, "\n\n" + vocabulary_table(ROOT) + "\n")

    def test_permissions_keep_read_and_write_separate(self):
        table = vocabulary_table(ROOT)
        self.assertIn("| `AT` | `read`", table)
        self.assertIn("| `AT+SET [<name>]` | `read`", table)
        self.assertIn("| `AT+SET <name> <value>` | `guarded`", table)
        self.assertIn(r"`AT+LOG FOLLOW 0\|1`", table)

    def test_migration_rejects_missing_form_and_wrong_owner(self):
        ledger = json.loads((ROOT / MIGRATION).read_text(encoding="utf-8"))
        for owner, verb, wrong_owner, expected in (
            ("provisioning", "CANCEL", None, r"provisioning: AT\+NET CANCEL"),
            ("platform", None, "net", r"platform: AT\+PLATFORM(?:;|$)"),
            ("call", "ACCEPT", "wechat", r"call: AT\+CALL ACCEPT"),
        ):
            with self.subTest(owner=owner):
                units = catalog(ROOT)
                declarations = units["module"][owner].commands
                row = next(entry for entry in declarations if entry.get("verb") == verb)
                declarations.remove(row)
                if wrong_owner == "net":
                    units["module"][wrong_owner].commands.append(row)
                if wrong_owner:
                    self.assertTrue(any(entry["command"] == row["command"] and entry["verb"] == verb
                                        for entry in vocabulary(units["module"].values())))
                with self.assertRaisesRegex(SelectionError, expected):
                    check_migration(ledger, units)

    def test_migration_compares_form_semantics_not_placeholder_spelling(self):
        self.assertEqual(usage_forms("<a> [<b> [<c>]]"), usage_forms("<input> [<0..4> [<6 digits>]]"))
        self.assertEqual(usage_forms("<0|1>"), usage_forms("1|0"))
        self.assertNotEqual(usage_forms("[FORCE]"), usage_forms("FORGET [FORCE]"))


if __name__ == "__main__":
    unittest.main()
