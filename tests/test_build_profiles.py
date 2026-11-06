import importlib.util
import json
import os
from pathlib import Path
import tempfile
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("xiaotai_build", ROOT / "scripts/xiaotai_build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class Profiles(unittest.TestCase):
    def test_szpi_firmware_preserves_validated_watchdog(self):
        cfg = build.configuration(build.select("lckfb/szpi-esp32s3"), "firmware")
        self.assertEqual(cfg["CONFIG_ESP_INT_WDT_TIMEOUT_MS"], "300")

    def test_dependency_lock_is_seeded_without_overwriting_existing_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            variant = {"name": "fixture"}
            out = root / "build"
            out.mkdir()
            with self.assertRaisesRegex(ValueError, "Missing product dependency lock"):
                build.seed_dependency_lock(variant, out, root)
            source = build.dependency_lock(variant, root)
            source.parent.mkdir(parents=True)
            source.write_bytes(b"locked input")
            build.seed_dependency_lock(variant, out, root)
            self.assertEqual((out / "dependencies.lock").read_bytes(), b"locked input")
            (out / "dependencies.lock").write_bytes(b"existing build")
            build.seed_dependency_lock(variant, out, root)
            self.assertEqual((out / "dependencies.lock").read_bytes(), b"existing build")

    def test_six_board_profiles(self):
        data = build.variants()
        self.assertTrue({"xiaotai-szpi-s3", "xiaotai-cores3-lite", "xiaotai-waveshare-p4-rev3",
                         "xiaotai-wt99p461-s1-1v1", "xiaotai-xmini-c3-v3", "xiaotai-mz28-ai-ppd"}
                        <= {v["name"] for v in data})
        self.assertTrue({"esp32s3", "esp32p4", "esp32c3"} <= {v["target"] for v in data})
        self.assertFalse(any(v["xiaotai"]["hardware_tested"] for v in data))

    def test_profiles_are_isolated(self):
        with patch.dict(os.environ, {}, clear=True):
            plans = [build.plan(v, "sdk-check") for v in build.variants()]
        self.assertEqual(len({p["build_dir"] for p in plans}), len(plans))
        self.assertEqual(len({p["sdkconfig"] for p in plans}), len(plans))
        self.assertTrue(all(Path(p["sdkconfig"]).parent == Path(p["build_dir"]) for p in plans))

    def test_target_conflict_rejected(self):
        variant = build.select("xmini/c3-v3")
        with patch.dict(os.environ, {"IDF_TARGET": "esp32p4"}):
            with self.assertRaisesRegex(ValueError, "conflicts"):
                build.plan(variant, "sdk-check")

    def test_diagnostics_build_isolated_and_explicit(self):
        item = build.select("lckfb/szpi-esp32s3")
        product = build.plan(item, "firmware")
        debug = build.plan(item, "firmware", {"development_console": True})
        self.assertNotEqual(product["build_dir"], debug["build_dir"])
        self.assertNotEqual(product["sdkconfig"], debug["sdkconfig"])
        self.assertNotEqual(product["config"].get("CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE"), "y")
        self.assertEqual(debug["config"]["CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE"], "y")
        self.assertFalse(debug["hardware_tested"])
        for board, stage, value in (("lckfb/szpi-esp32s3", "sdk-check", True),
                                    ("xmini/c3-v3", "firmware", True),
                                    ("lckfb/szpi-esp32s3", "firmware", 1)):
            with self.assertRaises(ValueError):
                build.plan(build.select(board), stage, {"development_console": value})

    def test_unregistered_board_has_no_fallback(self):
        with self.assertRaises(ValueError):
            build.select("espressif/esp32-s3-box-3")

    def test_path_traversal_rejected(self):
        with self.assertRaises(ValueError):
            build.select("../../../../xiaotai", "../sdkconfig")

    def test_pending_boards_cannot_build_firmware(self):
        for item in build.variants():
            if item["xiaotai"]["status"] == "pending":
                with self.assertRaisesRegex(ValueError, "migration pending"):
                    build.configuration(item, "firmware")

    def test_lightweight_profiles_preserve_features(self):
        xmini = build.select("xmini/c3-v3")
        mz28 = build.select("mz28-ai-ppd")
        self.assertEqual(xmini["xiaotai"]["audio"], "pcma8k")
        self.assertEqual(mz28["xiaotai"]["features"], ["wechat_voip"])
        for item in (xmini, mz28):
            cfg = build.configuration(item, "sdk-check")
            self.assertEqual(cfg["CONFIG_XIAOTAI_H5_VIDEO"], "n")
            self.assertNotEqual(cfg.get("CONFIG_SPIRAM"), "y")
        self.assertEqual(build.configuration(xmini, "sdk-check")["CONFIG_XIAOTAI_H5_STREAM"], "y")
        self.assertEqual(build.configuration(mz28, "sdk-check")["CONFIG_XIAOTAI_H5_STREAM"], "n")

    def test_feature_change_cannot_hide_missing_source_gates(self):
        item = build.select("xmini/c3-v3")
        for options in ({"ai": False}, {"h5_video": True}, {"ai": 1}, {"ota": True}):
            with self.assertRaises(ValueError):
                build.configuration(item, "sdk-check", options)
        self.assertEqual(build.configuration(item, "sdk-check", {"ai": True}),
                         build.configuration(item, "sdk-check"))

    def test_s3_sdk_check_preserves_libc_atomic_configuration(self):
        cfg = build.configuration(build.select("lckfb/szpi-esp32s3"), "sdk-check")
        self.assertEqual(cfg["CONFIG_SPIRAM"], "y")
        self.assertEqual(cfg["CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND"], "y")

    def test_effective_config_checked_including_strings(self):
        build.verify_config({"CONFIG_FOO": "y"}, {"CONFIG_FOO": "y", "CONFIG_OFF": "n"})
        for actual in ({"CONFIG_IDF_TARGET": '"esp32p4"'}, {}):
            with self.assertRaises(ValueError):
                build.verify_config(actual, {"CONFIG_IDF_TARGET": '"esp32c3"'})

    def test_kconfig_not_set_parser(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "sdkconfig"
            path.write_text('# CONFIG_TRACE is not set\nCONFIG_NAME="a=b"\n', encoding="utf-8")
            self.assertEqual(build.read_config(path), {"CONFIG_TRACE": "n", "CONFIG_NAME": '"a=b"'})

    def test_board_symbols_exist(self):
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        for item in build.variants():
            symbols = [s.split("=", 1)[0][7:] for s in item["sdkconfig_append"] if s.startswith("CONFIG_BOARD_TYPE_")]
            self.assertEqual(len(symbols), 1)
            self.assertIn("config " + symbols[0] + "\n", kconfig)

    def test_sdk_hashes(self):
        import hashlib
        for target in build.TARGETS:
            folder = ROOT / "components/tirtc_sdk/lib" / target
            contract = json.loads((folder / "contract.json").read_text(encoding="utf-8-sig"))
            self.assertEqual(hashlib.sha256((folder / "libTiRTC.a").read_bytes()).hexdigest(), contract["sha256"])
            self.assertEqual(contract["target"], target)
            self.assertFalse(contract["production_ready"])

    def test_build_requires_completed_preparation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "config").mkdir()
            path = root / "config/development-status.json"
            for state in ({}, {"phase": "validation", "preparation_complete": False},
                          {"phase": "validation", "preparation_complete": True, "pending": ["board"]}):
                path.write_text(json.dumps(state), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "Preparation is not complete"):
                    build.require_validation_phase(root)
            path.write_text(json.dumps({"phase": "validation", "preparation_complete": True, "pending": []}), encoding="utf-8")
            build.require_validation_phase(root)

    def test_preparation_checks_do_not_mark_hardware_ready(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "config").mkdir()
            state = {"phase": "preparation", "preparation_checks_authorized": True,
                     "preparation_complete": False, "hardware_tested": False,
                     "pending": ["runtime-checks"]}
            (root / "config/development-status.json").write_text(json.dumps(state))
            build.require_validation_phase(root)
            self.assertFalse(state["preparation_complete"])
            self.assertFalse(state["hardware_tested"])


if __name__ == "__main__":
    unittest.main()
