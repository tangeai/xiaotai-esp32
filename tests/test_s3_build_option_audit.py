"""Artifact feature checks for the lckfb S3 and CoreS3-Lite firmware."""
# Replaces legacy: tests/test_s3_build_option_audit.py
import unittest

from verify_build import S3_CONSOLE_SYMBOLS, verify_s3_build_options


RUNTIME = "/work/main/xiaotai/application/s3/starter_runtime/src/starter_runtime.c"
CAMERA = "/work/main/xiaotai/media/s3/starter_media/src/starter_camera.c"
CAMERA_OFF = "/work/main/xiaotai/media/s3/starter_media/camera_disabled.c"
VOICE = "\n".join(f"CONFIG_XIAOTAI_{name}=y"
                  for name in ("AI", "DEVICE_CALL", "WECHAT_VOIP", "ROOM", "H5_STREAM"))
M5_RUNTIME = "/work/main/xiaotai/application/s3/starter_runtime/src/starter_runtime.c"
M5_CONSOLE = "/work/main/xiaotai/input/cores3/starter_console/src/starter_console.c"
M5_MEDIA = "/work/main/xiaotai/media/cores3/starter_media/src/starter_media.c"
M5_BUSINESS = {"starter_button_start", "starter_product_start", "starter_tirtc_start"}
M5_DIAGNOSTICS = {"starter_console_start", "starter_runtime_ai_state_read",
                  "xiaotai_ui_capture", "lv_snapshot_take_to_draw_buf"}
M5_ENABLED = "CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE=y\nCONFIG_LV_USE_SNAPSHOT=y"


class S3BuildOptionAuditTests(unittest.TestCase):
    def audit(self, *, config=VOICE, compiled=(RUNTIME, CAMERA_OFF),
              symbols=(), expected=None, target="esp32s3"):
        if expected is None:
            expected = {"development_console": False, "ai_vision": False, "h5_video": False}
        return verify_s3_build_options({"stage": "firmware", "target": target, "board": "lckfb/szpi-esp32s3"},
                                       config, compiled, set(symbols), expected)

    def test_voice_only_options(self):
        result = self.audit()
        self.assertEqual(result, {"development_console": False, "ai_vision": False,
                                  "h5_video": False})

    def test_retired_trial_is_not_a_product_option(self):
        with self.assertRaisesRegex(ValueError, "boolean S3 build options"):
            self.audit(expected={"ai_pcma_trial": True})

    def test_one_video_option_keeps_camera(self):
        self.assertTrue(self.audit(
            config=VOICE + "\nCONFIG_XIAOTAI_H5_VIDEO=y",
            compiled=(RUNTIME.replace("/", "\\"), CAMERA.replace("/", "\\")),
            symbols={"xiaotai_board_camera_set_power"},
            expected={"h5_video": True, "ai_vision": False})["h5_video"])

    def test_uses_effective_options_and_rejects_leakage(self):
        config = VOICE + "\nCONFIG_XIAOTAI_DEVELOPMENT_CONSOLE=y"
        with self.assertRaisesRegex(ValueError, "Effective S3 option development_console differs"):
            self.audit(config=config)
        with self.assertRaisesRegex(ValueError, "Unexpected development console symbols"):
            self.audit(symbols={"xiaotai_ui_tap"})
        with self.assertRaisesRegex(ValueError, "Missing development console symbols"):
            self.audit(config=config, expected={"development_console": True},
                       symbols=S3_CONSOLE_SYMBOLS - {"starter_runtime_ai_state_read"})
        self.assertTrue(self.audit(config=config, expected={"development_console": True},
                                   symbols=S3_CONSOLE_SYMBOLS)["development_console"])

    def test_checks_camera_source_and_linked_power_entry(self):
        for compiled, symbols in (
            ((RUNTIME, CAMERA_OFF, CAMERA), set()),
            ((RUNTIME,), set()),
            ((RUNTIME, CAMERA_OFF), {"xiaotai_board_camera_set_power"}),
        ):
            with self.subTest(compiled=compiled, symbols=symbols):
                with self.assertRaisesRegex(ValueError, "camera source or power entry"):
                    self.audit(compiled=compiled, symbols=symbols)
        with self.assertRaisesRegex(ValueError, "camera source or power entry"):
            self.audit(config=VOICE + "\nCONFIG_XIAOTAI_AI_VISION=y",
                       compiled=(RUNTIME, CAMERA), expected={"ai_vision": True})

    def test_rejects_wrong_source_profile_or_missing_voice(self):
        with self.assertRaisesRegex(ValueError, "requires an ESP32-S3"):
            self.audit(target="esp32p4")
        with self.assertRaisesRegex(ValueError, "requires the lckfb S3 firmware"):
            self.audit(compiled=(CAMERA_OFF,))
        with self.assertRaisesRegex(ValueError, "missing required voice features"):
            self.audit(config=VOICE.replace("CONFIG_XIAOTAI_H5_STREAM=y", ""))

    def test_rejects_empty_unknown_and_nonboolean_options(self):
        for expected in ({}, [], {"unknown": True}, {"h5_video": 1}):
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(ValueError, "boolean S3 build options"):
                    self.audit(expected=expected)

    def test_cores3_console_profiles(self):
        record = {"stage": "firmware", "target": "esp32s3", "board": "m5stack/core-s3"}
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                expected = {"development_console": enabled}
                result = verify_s3_build_options(
                    record, M5_ENABLED if enabled else "",
                    (M5_RUNTIME, M5_MEDIA, M5_CONSOLE) if enabled else (M5_RUNTIME, M5_MEDIA),
                    M5_BUSINESS | M5_DIAGNOSTICS if enabled else M5_BUSINESS,
                    expected)
                self.assertEqual(result, expected)

    def test_cores3_rejects_console_drift(self):
        record = {"stage": "firmware", "target": "esp32s3", "board": "m5stack/core-s3"}
        for config, sources, symbols, expected, reason in (
            (M5_ENABLED, (M5_RUNTIME, M5_MEDIA, M5_CONSOLE), M5_BUSINESS | M5_DIAGNOSTICS,
             {"development_console": False}, "Effective S3 option"),
            (M5_ENABLED, (M5_RUNTIME, M5_MEDIA, M5_CONSOLE),
             M5_BUSINESS | (M5_DIAGNOSTICS - {"starter_runtime_ai_state_read"}),
             {"development_console": True}, "Missing development console symbols"),
            ("", (M5_RUNTIME, M5_MEDIA), M5_BUSINESS | {"starter_runtime_ai_state_read"},
             {"development_console": False}, "Unexpected development console symbols"),
            ("", (M5_RUNTIME, M5_MEDIA, M5_CONSOLE), M5_BUSINESS,
             {"development_console": False}, "source or LVGL snapshot"),
            ("CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE=y", (M5_RUNTIME, M5_MEDIA, M5_CONSOLE),
             M5_BUSINESS | M5_DIAGNOSTICS, {"development_console": True}, "source or LVGL snapshot"),
            ("", (M5_RUNTIME, M5_MEDIA), M5_BUSINESS - {"starter_button_start"},
             {"development_console": False}, "missing required runtime"),
            ("", (M5_RUNTIME, M5_MEDIA), M5_BUSINESS, {"h5_video": False}, "CoreS3-Lite audit only accepts"),
            ("", (RUNTIME, CAMERA_OFF), M5_BUSINESS, {"development_console": False}, "requires a CoreS3-Lite"),
            ("", (M5_RUNTIME, RUNTIME), M5_BUSINESS, {"development_console": False}, "requires a CoreS3-Lite"),
        ):
            with self.subTest(reason=reason):
                with self.assertRaisesRegex(ValueError, reason):
                    verify_s3_build_options(record, config, sources, symbols, expected)


if __name__ == "__main__":
    unittest.main()
