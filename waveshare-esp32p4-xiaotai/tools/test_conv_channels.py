"""Check the real configure-time fix; no firmware, devices, or cache writes."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RELATIVE = Path("tensorflow/lite/micro/kernels/esp_nn/conv.cc")
UPSTREAM = ROOT / "managed_components/espressif__esp-tflite-micro" / RELATIVE
FIX = ROOT / "components/starter_voice/conv_channels.cmake"
BEFORE = ".channels = 0, .extra = 0"
AFTER = ".channels = filter->dims->data[3], .extra = 0"


class ConvChannelsTest(unittest.TestCase):
    def configure(self, root, changed=False, missing_source=False):
        component = root / "component"
        source = component / RELATIVE
        source.parent.mkdir(parents=True)
        seed = UPSTREAM.read_bytes()
        source.write_bytes(seed + (b"\n// changed upstream\n" if changed else b""))
        selected = "other.cc" if missing_source else source.as_posix()
        (root / "other.cc").write_text("// fixture\n", encoding="utf-8")
        (root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.16)\nproject(conv_check NONE)\n'
            f'add_custom_target(tflm SOURCES "{selected}")\n'
            f'include("{FIX.as_posix()}")\n'
            f'xiaotai_fix_conv_channels(tflm "{component.as_posix()}" '
            '"${CMAKE_BINARY_DIR}/p4_tflm")\n'
            'if(NOT EXISTS "${CMAKE_BINARY_DIR}/p4_tflm/conv.cc")\n'
            'message(FATAL_ERROR "Convolution source must exist before generation")\nendif()\n'
            'get_target_property(actual tflm SOURCES)\n'
            'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/sources.txt" CONTENT "${actual}")\n',
            encoding="utf-8")
        result = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build"),
                                 "-G", "Ninja"], capture_output=True, text=True, encoding="utf-8")
        return result, source, seed

    def test_prepare_and_eval_and_repeat_configure(self):
        original_hash = hashlib.sha256(UPSTREAM.read_bytes()).hexdigest()
        for suffix in ("spaces included", "\u5c0f\u949b"):
            with self.subTest(path=suffix), tempfile.TemporaryDirectory() as directory:
                root = Path(directory) / suffix
                root.mkdir()
                result, source, seed = self.configure(root)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                generated = root / "build/p4_tflm/conv.cc"
                expected = seed.decode("utf-8").replace("\r\n", "\n").replace(BEFORE, AFTER)
                self.assertEqual(generated.read_text(encoding="utf-8"), expected)
                self.assertEqual(expected.count(AFTER), 2)
                self.assertNotIn(BEFORE, expected)
                self.assertEqual(source.read_bytes(), seed)
                # CMake writes UTF-8 even when the Windows locale uses GBK.
                self.assertEqual((root / "build/sources.txt").read_text(encoding="utf-8"), generated.as_posix())
                stamp = generated.stat().st_mtime_ns
                again = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build")],
                                       capture_output=True, text=True, encoding="utf-8")
                self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
                self.assertEqual(generated.stat().st_mtime_ns, stamp)
        self.assertEqual(hashlib.sha256(UPSTREAM.read_bytes()).hexdigest(), original_hash)

    def test_upstream_change_requires_review(self):
        with tempfile.TemporaryDirectory() as directory:
            result, _, _ = self.configure(Path(directory), changed=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("TFLM conv.cc changed", result.stdout + result.stderr)

    def test_wrong_target_requires_review(self):
        with tempfile.TemporaryDirectory() as directory:
            result, _, _ = self.configure(Path(directory), missing_source=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no longer compiles", result.stdout + result.stderr)

    def test_retired_switches_absent(self):
        for name in ("APP_AUDIO_AEC_ENABLE", "APP_AI_CHAT_VIDEO_ENABLE",
                     "APP_WECHAT_VOIP_LOCAL_VIDEO_ENABLE", "APP_WECHAT_VOIP_REMOTE_VIDEO_ENABLE"):
            self.assertNotIn("config " + name, (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8"))
            self.assertNotIn("CONFIG_" + name, (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
