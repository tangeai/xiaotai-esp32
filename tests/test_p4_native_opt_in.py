"""Check the separate cutover state and native hardware profile of each P4."""

import json
from pathlib import Path
import unittest

from test_native_build_entry import WT99, invoke


BOARD = "waveshare/esp32-p4-wifi6-touch-lcd-3.5"


class P4NativeRoutingTest(unittest.TestCase):
    def test_waveshare_default_is_the_locked_native_build_without_a_legacy_entry(self):
        default = invoke(BOARD, "--dry-run")
        legacy = invoke(BOARD, "--entry", "legacy", "--dry-run")
        sdk = invoke(BOARD, "--entry", "sdk", "--dry-run")
        for result in (default, sdk):
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotEqual(legacy.returncode, 0)
        self.assertEqual(json.loads(default.stdout), json.loads(sdk.stdout))
        native = json.loads(sdk.stdout)
        self.assertEqual((native["entry"], native["board"], native["target"]), ("sdk", BOARD, "esp32p4"))
        self.assertEqual(native["idf_environment"]["version"], "6.0.3")
        self.assertEqual(native["tirtc_package"], "6.0/generic-esp32p4")
        self.assertEqual(Path(native["build_dir"]).name, "sdk-firmware")
        self.assertEqual(native["config"]["CONFIG_CAMERA_OV5647_MIPI_DEFAULT_FMT_RAW10_1280X960_BINNING_45FPS"], "y")
        self.assertNotEqual(native["config"].get("CONFIG_XIAOTAI_UI_CAPTURE"), "y")
        opted = invoke(BOARD, "--entry", "sdk", "--idf", "5.5", "--dry-run")
        self.assertEqual(opted.returncode, 0, opted.stderr)

    def test_wt99_public_alias_defaults_to_locked_sdk(self):
        default = invoke(WT99, "--dry-run")
        legacy = invoke(WT99, "--entry", "legacy", "--dry-run")
        sdk = invoke(WT99, "--entry", "sdk", "--dry-run")
        for result in (default, sdk):
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotEqual(legacy.returncode, 0)
        self.assertEqual(json.loads(default.stdout), json.loads(sdk.stdout))
        native = json.loads(sdk.stdout)
        self.assertEqual((native["entry"], native["board"], native["target"]), ("sdk", "wt/wt99p461-s1", "esp32p4"))
        self.assertEqual(native["idf_environment"]["version"], "6.0.3")
        self.assertEqual(native["tirtc_package"], "6.0/generic-esp32p4")
        self.assertEqual(Path(native["build_dir"]).name, "sdk-firmware")
        self.assertNotEqual(native["config"].get("CONFIG_XIAOTAI_UI_CAPTURE"), "y")
        for key in ("CONFIG_ESP_HOSTED_CP_TARGET_ESP32C61", "CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SPI",
                    "CONFIG_CAMERA_SC2336_MIPI_DEFAULT_FMT_RAW8_1024X600_30FPS"):
            self.assertEqual(native["config"][key], "y")

    def test_wt99_idf55_defaults_to_the_sdk_entry(self):
        default = invoke(WT99, "--idf", "5.5", "--dry-run")
        opted = invoke(WT99, "--idf", "5.5", "--entry", "sdk", "--dry-run")
        for result in (default, opted):
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(default.stdout), json.loads(opted.stdout))
        self.assertNotEqual(invoke(WT99, "--idf", "5.5", "--entry", "legacy", "--dry-run").returncode, 0)


if __name__ == "__main__":
    unittest.main()
