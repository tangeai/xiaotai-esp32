import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

# Replaces legacy: tests/test_change_impact.py

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("change_impact", ROOT / "scripts/check_change_impact.py")
impact = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(impact)


class ImpactTests(unittest.TestCase):
    def setUp(self):
        self.products = impact.load_products(ROOT)

    def users(self, path):
        return impact.analyze([path], self.products)["affected_products"]

    def sdk_users(self, path):
        return impact.analyze([path], impact.load_sdk_products(ROOT), graph="sdk")["affected_products"]

    def test_sdk_board_and_packages_reach_their_products(self):
        self.assertEqual(self.sdk_users("boards/m5stack/core-s3/board.cc"), ["xiaotai-cores3-lite"])
        self.assertEqual(self.sdk_users("components/tirtc_sdk/contracts/6.0/esp32s3.json"),
                         ["xiaotai-cores3-lite", "xiaotai-szpi-s3"])
        self.assertEqual(self.sdk_users("components/tirtc_sdk/contracts/6.0/esp32p4.json"),
                         ["xiaotai-waveshare-p4-rev3", "xiaotai-wt9932p4c61-tiny", "xiaotai-wt99p461-s1-1v1"])
        self.assertEqual(self.sdk_users("components/tirtc_sdk/contracts/5.5/esp32s3.json"),
                         ["xiaotai-cores3-lite", "xiaotai-szpi-s3"])

    def test_representative_keeps_all_consumer_checks(self):
        result = impact.analyze(["main/xiaotai/media/shared/packet.h"], self.products, "xiaotai-szpi-s3")
        self.assertFalse(result["hardware_selection_required"])
        self.assertEqual(result["selected_hardware_products"], ["xiaotai-szpi-s3"])

    def test_shared_all(self):
        self.assertEqual(set(self.users("main/xiaotai/media/shared/packet.h")), set(self.products))

    def test_unknown_sdk_file_asks_for_review(self):
        # No product links a file outside its package's contract or tree.
        result = impact.analyze(["components/tirtc_sdk/contracts/notes.txt"], self.products)
        self.assertEqual(result["selected_hardware_products"], [])
        self.assertTrue(result["manual_review_required"])

    def test_contract_all(self):
        self.assertEqual(len(self.users("config/adapter-contracts.json")), len(self.products))
        self.assertEqual(len(self.users("config/capabilities/psram.defaults")), len(self.products))

    def test_unknown_fail_closed(self):
        result = impact.analyze(["new-stack/new.c"], self.products)
        self.assertTrue(result["manual_review_required"])
        self.assertEqual(set(result["affected_products"]), set(self.products))
        self.assertFalse(result["tests_executed"])

    def test_document_only(self):
        result = impact.analyze(["docs/new-board.md"], self.products)
        self.assertEqual(result["matrix"], [])
        self.assertIsNone(result["host_command"])

    def test_tools_require_dry_run_not_hardware(self):
        result = impact.analyze(["scripts/check_change_impact.py"], self.products)
        self.assertTrue(result["matrix"])
        self.assertTrue(all(not p["firmware_build_required"] and not p["hardware_checks"] for p in result["matrix"]))

    def test_shared_paths_do_not_schedule_all_boards(self):
        result = impact.analyze(["main/xiaotai/media/shared/packet.h"], self.products)
        self.assertTrue(result["hardware_selection_required"])
        self.assertEqual(result["selected_hardware_products"], [])
        self.assertTrue(all(not p["hardware_run_required"] and not p["hardware_checks"]
                            for p in result["matrix"]))

    def test_sdk_identity_does_not_expand_pressure_testing(self):
        result = impact.analyze(["components/tirtc_sdk/contracts/6.0/esp32p4.json"], self.products)
        self.assertTrue(result["sdk_change_requires_authorization"])
        self.assertEqual(result["selected_hardware_products"], [])
        self.assertTrue(all(not p["hardware_checks"] for p in result["matrix"]))
        self.assertTrue(all(p["sdk_extra_checks"] == ["approved_dependency_identity",
            "archive_headers_abi", "elf_link_identity"] for p in result["matrix"]))

    def test_bad_representative_is_not_silently_accepted(self):
        for paths, name in ((["docs/guide.md"], "xiaotai-szpi-s3"),
                            (["scripts/check_change_impact.py"], "xiaotai-szpi-s3"),
                            (["main/xiaotai/media/shared/packet.h"], "unknown-board")):
            with self.subTest(paths=paths, name=name), self.assertRaises(ValueError):
                impact.analyze(paths, self.products, name)

    def test_path_boundaries(self):
        self.assertEqual(impact.normalized("main\\boards\\new\\board.c"), "main/boards/new/board.c")
        for path in ("../x", "/tmp/x", "C:/x", "a/../b", ""):
            with self.assertRaises(ValueError):
                impact.normalized(path)
        self.assertFalse(impact.inside("main/boards/test-two/a.c", "main/boards/test"))

    def test_missing_manifest_is_error(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "products").mkdir(parents=True)
            with self.assertRaisesRegex(ValueError, "No SDK product manifests"):
                impact.load_products(root)

    def test_git_move_delete_add_and_untracked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def git(*args):
                subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True)
            git("init")
            (root / "old board.c").write_text("source")
            git("add", ".")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "fixture")
            git("mv", "old board.c", "new board.c")
            (root / "extra.h").write_text("extra")
            self.assertEqual(impact.git_paths(root), ["extra.h", "new board.c", "old board.c"])

    def test_cli_unknown_requires_review(self):
        result = subprocess.run([sys.executable, str(ROOT / "scripts/check_change_impact.py"),
                                 "--files", "unknown/new.c", "--strict"], capture_output=True)
        self.assertEqual(result.returncode, 2)
        self.assertTrue(json.loads(result.stdout)["manual_review_required"])

    def test_cli_never_overwrites_report(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "report.json"
            report.write_text("keep evidence")
            result = subprocess.run([sys.executable, str(ROOT / "scripts/check_change_impact.py"),
                                     "--files", "README.md", "--output", str(report)], capture_output=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(report.read_text(), "keep evidence")

    def test_base_is_not_git_option(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(["git", "init", directory], check=True, capture_output=True)
            with self.assertRaises(subprocess.CalledProcessError):
                impact.git_paths(Path(directory), "--output=unexpected")
            self.assertFalse((Path(directory) / "unexpected").exists())


class SdkImpactTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.products = impact.load_sdk_products(ROOT)

    def analyze(self, *paths, representative=None):
        return impact.analyze(paths, self.products, representative, graph="sdk")

    def test_product_and_board_are_owned_by_the_real_sdk_manifests(self):
        for path, kind in (("boards/m5stack/core-s3/board.cc", "board"),
                           ("products/xiaotai-cores3-lite/product.json", "product")):
            with self.subTest(path=path):
                result = self.analyze(path)
                self.assertEqual(result["affected_products"], ["xiaotai-cores3-lite"])
                self.assertEqual(result["selected_hardware_products"], ["xiaotai-cores3-lite"])
                self.assertEqual(result["changed_files"][0]["kind"], kind)
                self.assertFalse(result["manual_review_required"])

    def test_part_consumers_follow_sdk_graph_instead_of_all_products(self):
        cases = {
            "sdk/parts/xiaotai_part_camera_dvp/esp-idf/camera_dvp_io.c":
                {"xiaotai-cores3-lite", "xiaotai-szpi-s3"},
            "sdk/parts/xiaotai_part_coprocessor/esp-idf/coprocessor_link.c":
                {"xiaotai-waveshare-p4-rev3", "xiaotai-wt9932p4c61-tiny", "xiaotai-wt99p461-s1-1v1"},
        }
        for path, expected in cases.items():
            with self.subTest(path=path):
                result = self.analyze(path, representative=min(expected))
                self.assertEqual(set(result["affected_products"]), expected)
                self.assertEqual(result["selected_hardware_products"], [min(expected)])
                self.assertFalse(result["manual_review_required"])

    def test_shared_sdk_changes_keep_all_consumers_and_targeted_hardware(self):
        result = self.analyze("sdk/xiaotai_framework/src/framework.c", "boards/m5stack/core-s3/board.cc",
                              representative="xiaotai-szpi-s3")
        self.assertEqual(set(result["affected_products"]), set(self.products))
        self.assertEqual(result["selected_hardware_products"], ["xiaotai-cores3-lite", "xiaotai-szpi-s3"])
        self.assertFalse(result["tests_executed"])

    def test_unknown_sdk_inputs_and_legacy_paths_fail_closed(self):
        for path in ("boards/m5stack/core-s3-extra/board.cc", "products/new-product/product.json",
                     "sdk/unresolved-part/implementation.c", "main/xiaotai/media/shared/packet.h"):
            with self.subTest(path=path):
                result = self.analyze(path)
                self.assertEqual(set(result["affected_products"]), set(self.products))
                self.assertTrue(result["manual_review_required"])
                self.assertEqual(result["selected_hardware_products"], [])

    def test_sdk_metadata_does_not_narrow_to_only_current_consumers(self):
        for path in ("sdk/parts/xiaotai_part_camera_dvp/CMakeLists.txt",
                     "sdk/modules/xiaotai_mod_ai_vision/module.json", "scripts/sdk_selection.py"):
            with self.subTest(path=path):
                result = self.analyze(path)
                self.assertEqual(set(result["affected_products"]), set(self.products))
                self.assertTrue(result["manual_review_required"])
                self.assertTrue(all(row["firmware_build_required"] for row in result["matrix"]))

    def test_preserves_sdk_identity_review_and_extra_idf_line(self):
        path = "components/tirtc_sdk/contracts/5.5/esp32s3.json"
        result = self.analyze(path)
        self.assertEqual(result["affected_products"], ["xiaotai-cores3-lite", "xiaotai-szpi-s3"])
        self.assertTrue(result["sdk_change_requires_authorization"])
        self.assertEqual(result["selected_hardware_products"], [])
        for row in result["matrix"]:
            self.assertEqual(row["extra_idf_lines"], ["5.5"])
            self.assertTrue(row["firmware_build_required"])
            self.assertEqual(row["sdk_extra_checks"], ["approved_dependency_identity", "archive_headers_abi",
                                                      "elf_link_identity"])
            self.assertTrue(all(command[1] == "scripts/sdk_selection.py" for command in row["commands"]))

    def test_retired_native_input_no_longer_maps_to_a_product(self):
        result = self.analyze("profiles/dependency-locks/xiaotai-mz28-ai-ppd-native.lock")
        self.assertEqual(set(result["affected_products"]), set(self.products))
        self.assertNotIn("xiaotai-mz28-ai-ppd", self.products)
        self.assertTrue(result["manual_review_required"])
        self.assertEqual(result["selected_hardware_products"], [])

    def test_cli_uses_only_sdk_selection_previews_and_lists_all_variants(self):
        path = "products/xiaotai-szpi-s3/product.json"
        result = subprocess.run([sys.executable, str(ROOT / "scripts/check_change_impact.py"),
                                 "--graph", "sdk", "--files", path, "--strict"],
                                capture_output=True, check=True)
        report = json.loads(result.stdout)
        self.assertEqual(report["graph"], "sdk")
        self.assertEqual(report["host_command"], ["python", "tests/test_products.py", "--require-products"])
        self.assertEqual(report["affected_products"], ["xiaotai-szpi-s3"])
        commands = report["matrix"][0]["commands"]
        base = ["python", "scripts/sdk_selection.py", "--product", path]
        self.assertEqual(commands, [base, base + ["--variant", "diagnostics"], base + ["--variant", "no-video"]])

    def test_native_build_admission_changes_are_not_tooling_only(self):
        paths = ["scripts/xiaotai_build_native.py", "scripts/idf_environment.py"]
        for path in paths:
            with self.subTest(path=path):
                self.assertTrue((ROOT / path).is_file())
                result = subprocess.run([sys.executable, str(ROOT / "scripts/check_change_impact.py"),
                                         "--graph", "sdk", "--files", path, "--strict"], capture_output=True)
                self.assertEqual(result.returncode, 2, result.stderr.decode())
                report = json.loads(result.stdout)
                self.assertEqual(set(report["affected_products"]), set(self.products))
                self.assertEqual(report["changed_files"][0]["kind"], "contract")
                self.assertTrue(report["manual_review_required"])
                self.assertTrue(all(row["firmware_build_required"] for row in report["matrix"]))

    def test_native_lock_and_profile_ownership(self):
        szpi = "xiaotai-szpi-s3"
        p4 = "xiaotai-waveshare-p4-rev3"
        cases = {
            f"profiles/dependency-locks/{szpi}-native.lock": szpi,
            f"profiles/dependency-locks/{szpi}--no-video.lock": szpi,
            f"profiles/dependency-locks/{p4}-native.lock": p4,
            f"profiles/native-firmware/{p4}.json": p4,
            f"profiles/{p4}-native.yml": p4,
        }
        for path, name in cases.items():
            with self.subTest(path=path):
                self.assertTrue((ROOT / path).is_file())
                report = self.analyze(path)
                self.assertEqual(report["affected_products"], [name])
                self.assertEqual(report["changed_files"][0]["kind"], "dependencies")
                self.assertEqual(report["selected_hardware_products"], [])
                self.assertTrue(report["matrix"][0]["firmware_build_required"])
                self.assertEqual(report["matrix"][0]["extra_idf_lines"], [])
        undeclared = self.analyze(f"profiles/dependency-locks/{szpi}--diagnostics.lock")
        self.assertTrue(undeclared["manual_review_required"])
        self.assertEqual(set(undeclared["affected_products"]), set(self.products))

    def test_reported_sdk_selection_commands_resolve_all_declared_variants(self):
        report = self.analyze("sdk/xiaotai_framework/src/framework.c")
        for row in report["matrix"]:
            for command in row["commands"]:
                with self.subTest(command=command):
                    output = subprocess.check_output([sys.executable, *command[1:]], cwd=ROOT)
                    selection = json.loads(output)
                    self.assertEqual((selection["product"], selection["board"], selection["target"]),
                                     (row["product"], row["board"], row["target"]))
                    self.assertEqual(command[0], "python")

    def test_native_optional_line_inputs_point_only_to_their_product(self):
        for product in ("xiaotai-cores3-lite", "xiaotai-szpi-s3", "xiaotai-waveshare-p4-rev3", "xiaotai-wt99p461-s1-1v1"):
            for path in (f"profiles/dependency-locks/idf5.5/{product}-native.lock",
                         f"profiles/native-firmware/idf5.5/{product}.json", f"profiles/{product}-idf5.5-native.yml"):
                with self.subTest(path=path):
                    self.assertTrue((ROOT / path).is_file())
                    report = self.analyze(path)
                    self.assertEqual(report["affected_products"], [product])
                    self.assertEqual(report["changed_files"][0]["kind"], "dependencies")
                    self.assertEqual(report["selected_hardware_products"], [])
                    self.assertTrue(report["matrix"][0]["firmware_build_required"])
                    self.assertEqual(report["matrix"][0]["extra_idf_lines"], ["5.5"])

if __name__ == "__main__":
    unittest.main()
