# SPDX-License-Identifier: MIT
"""Exercise the legacy regression migration ledger and cutover gate."""

import json
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

import check_migration as migration


class MigrationLedgerTests(unittest.TestCase):
    def test_live_inventory(self):
        ledger = json.loads((migration.ROOT / "tests/migration.json").read_text(encoding="utf-8"))
        counts, errors = migration.validate(ledger)
        self.assertEqual(errors, [])
        self.assertEqual(counts["pending"] + counts["complete"] + counts["retired"], len(ledger["tests"]))
        self.assertEqual(counts["retired_pending"] + counts["retired_complete"], counts["retired"])

    def test_missing_duplicate_and_unknown_legacy_entry(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            row = self.make_fixture(root)
            unknown = dict(row, case="no_such_case")
            expected = {"duplicate tests/test_example.py", "unknown legacy test tests/test_example.py::no_such_case",
                        f"missing {migration.VOICE}::test_fixture"}
            (root / migration.VOICE).parent.mkdir()
            (root / migration.VOICE).write_text(
                'def test_fixture(): pass\n'
                'def test_not_run(): pass\n'
                'if __name__ == "__main__":\n    test_fixture()\n', encoding="utf-8",
            )
            # Keep this fixture independent of the real manual CMake groups.
            def discovered(path):
                self.assertEqual(path, root)
                return {
                    ("tests/test_example.py", None): "auto",
                    **{(migration.VOICE, case): "auto" for case in migration.voice_cases(root / migration.VOICE)},
                }
            with patch.object(migration, "inventory", discovered):
                _, errors = migration.validate({"schema": 1, "tests": [row, row, unknown]}, root)
            self.assertEqual(set(errors), expected)

    def test_pending_blocks_cutover_and_complete_requires_registered_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            row = self.make_fixture(root)
            data = {"schema": 1, "tests": [row]}
            with patch.object(migration, "inventory", return_value={(row["path"], None): "auto"}):
                self.assertEqual(migration.validate(data, root)[1], [])
                self.assertEqual(migration.validate(data, root, require_complete=True)[1],
                                 ["cutover blocked: 1 test migrations pending"])
                row["status"] = "complete"
                errors = migration.validate(data, root)[1]
                self.assertIn("unregistered destination", errors[0])
                target = root / "sdk/example/test"
                target.mkdir(parents=True)
                (target / "CMakeLists.txt").write_text(
                    "foreach(name other regression)\n"
                    "  xiaotai_host_test(example_${name} SOURCES ${name}_test.c LINK example)\n"
                    "endforeach()\n"
                    "add_test(NAME example_second_case COMMAND example_regression second)\n", encoding="utf-8",
                )
                fixture = target / "regression_test.c"
                fixture.write_text("int main(void) { return 0; }\n", encoding="utf-8")
                self.assertIn("missing destination evidence marker", migration.validate(data, root)[1][0])
                fixture.write_text("// Replaces legacy: tests/test_example.py\n"
                                   "int main(void) { return 0; }\n", encoding="utf-8")
                self.assertEqual(migration.validate(data, root, require_complete=True)[1], [])
                row["targets"][0]["test"] = "example_second_case"
                self.assertEqual(migration.validate(data, root, require_complete=True)[1], [])

    def test_product_gate_counts_common_and_selected_and_blocks_unclassified(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            common = self.make_fixture(root)
            product, other = self.make_products(root)
            common["scope"] = {"common": True, "basis": "Exercises the shared ABI on all products."}
            selected = self.make_row(root, "selected", {"products": [product], "basis": "Uses product's board factory."})
            outside = self.make_row(root, "outside", {"products": [other], "basis": "Uses other product's board factory."})
            unresolved = self.make_row(root, "unresolved")
            data = {"schema": 1, "tests": [common, selected, outside, unresolved]}
            required = {(row["path"], None): "auto" for row in data["tests"]}
            with patch.object(migration, "inventory", return_value=required):
                counts, errors = migration.validate(data, root, product=product)
                self.assertEqual(errors, [])
                self.assertEqual((counts["scoped_pending"], counts["outside_scope"], counts["unclassified"]), (2, 1, 1))
                self.assertEqual(migration.validate(data, root, require_complete=True, product=product)[1], [
                    f"cutover blocked for {product}: 1 rows lack reviewed scope",
                    f"cutover blocked for {product}: 2 test migrations pending",
                ])
                unresolved["scope"] = outside["scope"]
                for row in (common, selected):
                    row.update(status="complete", disposition="retire", targets=[], reason="Obsolete fixture.")
                self.assertEqual(migration.validate(data, root, require_complete=True, product=product)[1], [])
                self.assertEqual(migration.validate(data, root, require_complete=True)[1],
                                 ["cutover blocked: 2 test migrations pending"])

    def test_product_scope_cannot_hide_invalid_or_missing_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            row = self.make_fixture(root)
            product, other = self.make_products(root)
            data = {"schema": 1, "tests": [row]}
            with patch.object(migration, "inventory", return_value={(row["path"], None): "auto"}):
                with self.assertRaisesRegex(ValueError, "unknown product"):
                    migration.validate(data, root, product="typo")
                for invalid in (None, {}, {"common": False, "basis": "fixture"},
                                {"products": [], "basis": "fixture"}, {"products": [other, other], "basis": "fixture"},
                                {"products": ["typo"], "basis": "fixture"}, {"products": [other], "basis": " "},
                                {"products": [other], "common": True, "basis": "fixture"}):
                    with self.subTest(invalid=invalid):
                        row["scope"] = invalid
                        self.assertIn("invalid scope", migration.validate(data, root)[1][0])
                row["scope"] = {"products": [other], "basis": "Other product's board factory only."}
                self.assertEqual(migration.validate(data, root, product=product, require_complete=True)[1],
                                 [f"cutover blocked for {product}: no reviewed product or common rows"])
                row["status"] = "complete"
                self.assertIn("unregistered destination", migration.validate(data, root, product=product)[1][0])
                data["tests"] = []
                self.assertIn("missing tests/test_example.py", migration.validate(data, root, product=product)[1])

    def test_retired_fixture_retains_legacy_source_and_is_not_a_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            row = self.make_fixture(root)
            product, board = "xiaotai-old", "main/boards/old/board.json"
            (root / board).parent.mkdir(parents=True)
            (root / board).write_text(json.dumps({"products": [{"name": product}]}), encoding="utf-8")
            row.update(status="retired", disposition="retire", targets=[], reason="Product retired by user.",
                       retirement={"product": product, "decision": "2026-11-20", "legacy_board": board,
                                   "previous_status": "pending"})
            data = {"schema": 1, "tests": [row]}
            with patch.object(migration, "inventory", return_value={(row["path"], None): "auto"}):
                counts, errors = migration.validate(data, root, require_complete=True)
                self.assertEqual(errors, [])
                self.assertEqual((counts["retired"], counts["retired_pending"], counts["complete"]), (1, 1, 0))
                selected, _ = self.make_products(root)
                scoped_counts, _ = migration.validate(data, root, product=selected)
                for key in ("unclassified", "outside_scope", "scoped_pending", "scoped_complete"):
                    self.assertEqual(scoped_counts[key], 0, key)
                active = root / "products" / product / "product.json"
                active.parent.mkdir(parents=True)
                active.touch()
                self.assertIn("absent from the active SDK graph", migration.validate(data, root)[1][0])
                active.unlink()
                row["scope"] = {"common": True}
                self.assertIn("no destinations or active scope", migration.validate(data, root)[1][0])
                del row["scope"]
                row["retirement"]["decision"] = "2026-13-01"
                self.assertIn("invalid product retirement", migration.validate(data, root)[1][0])
                row["retirement"]["decision"] = "2026-11-20"
                (root / row["path"]).unlink()
                self.assertIn("missing legacy test source", migration.validate(data, root)[1][0])

    def test_frozen_inventory_survives_deleted_legacy_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            row = self.make_fixture(root)
            retired = {"path": "tests/old/test_gone.c", "source": "old", "case": "gone", "status": "retired",
                       "disposition": "retire", "packages": ["BI-7"], "targets": [], "reason": "Retired.",
                       "retirement": {"product": "xiaotai-gone", "decision": "2026-11-20",
                                      "legacy_board": "main/boards/gone/board.json", "previous_status": "pending"}}
            frozen = {"schema": 1, "commit": "0" * 40,
                      "tests": [[row["path"], None, "auto"], [retired["path"], "gone", "old"]],
                      "legacy_boards": {"main/boards/gone/board.json": ["xiaotai-gone"]}}
            (root / migration.FROZEN).write_text(json.dumps(frozen), encoding="utf-8")
            (root / row["path"]).unlink()
            self.assertEqual(migration.validate({"schema": 1, "tests": [row, retired]}, root)[1], [])
            frozen["legacy_boards"] = {}
            (root / migration.FROZEN).write_text(json.dumps(frozen), encoding="utf-8")
            self.assertIn("invalid product retirement",
                          migration.validate({"schema": 1, "tests": [row, retired]}, root)[1][0])

    @staticmethod
    def make_products(root):
        products = "xiaotai-one", "xiaotai-two"
        for name in products:
            directory = root / "products" / name
            directory.mkdir(parents=True)
            (directory / "product.json").write_text("{}", encoding="utf-8")
        return products

    @staticmethod
    def make_row(root, name, scope=None):
        path = f"tests/test_{name}.py"
        (root / path).touch()
        row = {"path": path, "source": "auto", "status": "pending", "disposition": "retire",
               "packages": ["BI-7"], "targets": [], "reason": "Obsolete fixture."}
        if scope is not None:
            row["scope"] = scope
        return row

    @staticmethod
    def make_fixture(root):
        (root / "tests").mkdir()
        (root / "tests/test_example.py").touch()
        return {"path": "tests/test_example.py", "source": "auto", "disposition": "adapt",
                "status": "pending", "packages": ["BI-7"],
                "targets": [{"component": "sdk/example", "test": "example_regression", "package": "BI-7"}]}


if __name__ == "__main__":
    unittest.main()
