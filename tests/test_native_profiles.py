import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from xiaotai_build import sdk_directory, select, verify_config


class NativeProfiles(unittest.TestCase):
    def test_mini_archive_is_only_selected_for_xmini(self):
        mini = sdk_directory(select("xmini/c3-v3"))
        generic = sdk_directory(select("mz28-ai-ppd"))
        self.assertNotEqual(mini, generic)
        self.assertIn("variants/xmini", mini.as_posix())
        self.assertTrue(generic.as_posix().endswith("lib/esp32c3"))
        lock = json.loads((mini / "contract.json").read_text())
        self.assertEqual(lock["xmini_contract"]["pending_command_requested_byte_budget"], 16384)

    def test_non_product_wake_model_rejected(self):
        with self.assertRaisesRegex(ValueError, "Non-product wake"):
            verify_config({"CONFIG_SR_WN_WN9_NI3HAO3XIAO3ZHI4_TTS": "y"}, {})

    def test_selected_source_graph_is_present_and_isolated(self):
        profiles = list((ROOT / "main/boards").rglob("source-profile.json"))
        self.assertGreaterEqual(len(profiles), 5)
        self.assertTrue({"cores3", "p4", "wt99", "xmini", "mz28"}
                        <= {json.loads(path.read_text())["id"] for path in profiles})
        for path in profiles:
            profile = json.loads(path.read_text())
            selected = select(path.parent.relative_to(ROOT / "main/boards").as_posix())
            self.assertEqual(profile["id"], selected["xiaotai"]["profile"])
            self.assertEqual(profile["target"], selected["target"])
            self.assertTrue((ROOT / profile["application"] / profile["entry"]).is_file())
            names = profile["components"]
            for name, folder in names.items():
                resolved = (ROOT / folder).resolve()
                self.assertTrue(resolved.is_relative_to(ROOT.resolve()))
                self.assertTrue((resolved / "CMakeLists.txt").is_file(), name)
            if profile["target"] == "esp32c3":
                self.assertFalse({"starter_media", "starter_voice", "starter_runtime", "lvgl"} & names.keys())
            if profile["id"] == "wt99":
                self.assertNotIn("esp32_p4_wifi6_touch_lcd_35", names)
                self.assertIn("wt99p461_s1", names)

    def test_actual_cpp_native_handoff_retains_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "application"
            subprocess.run(["c++", "-std=c++17", "-DXIAOTAI_NATIVE_PROFILE=1",
                "-I", str(ROOT / "tests/native_application"),
                "-I", str(ROOT / "main/boards/common"),
                "-I", str(ROOT / "main/xiaotai/application"),
                str(ROOT / "main/xiaotai/application/application.cc"),
                str(ROOT / "tests/native_application/application_test.cc"),
                "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_xmini_sdk_cannot_silently_disable_pending_close(self):
        header = (ROOT / "main/xiaotai/transport/xmini/xmini_rtc/include/xmini_rtc.h").read_text()
        self.assertIn("#error", header)
        self.assertIn("TIRTC_PENDING_CLOSE_CONTRACT != 1", header)
        recipe = (ROOT / "main/xiaotai/transport/xmini/xmini_rtc/CMakeLists.txt").read_text()
        self.assertIn("pending_command_requested_byte_budget", recipe)
        self.assertIn("diagnostic_header_sha256", recipe)


if __name__ == "__main__":
    unittest.main()
