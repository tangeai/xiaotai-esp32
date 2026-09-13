"""Check the pinned dependency adaptation without compiling firmware."""
import difflib
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "managed_components/espressif__esp-tflite-micro/tensorflow/lite/micro/kernels/esp_nn/conv.cc"
PATCH = ROOT / "cmake/tflm_conv_channels.cmake"


class ConvChannelsTest(unittest.TestCase):
    def test_only_filter_metadata_changes_and_dependency_is_untouched(self):
        original_bytes = SOURCE.read_bytes()
        original = SOURCE.read_text()
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "conv.cc"
            script = Path(tmp) / "run.cmake"
            script.write_text(f'include("{PATCH.as_posix()}")\nstarter_patch_tflm_conv("{SOURCE.as_posix()}" "{out.as_posix()}")\n')
            subprocess.run(["cmake", "-P", str(script)], check=True)
            lines = list(difflib.ndiff(original.splitlines(), out.read_text().splitlines()))
            removed = [s[2:].strip() for s in lines if s.startswith("- ")]
            added = [s[2:].strip() for s in lines if s.startswith("+ ")]
            self.assertEqual(removed, [".channels = 0, .extra = 0"] * 2)
            self.assertEqual(added, [".channels = filter_input_channels, .extra = 0",
                                     ".channels = filter_shape.Dims(3), .extra = 0"])
            before = out.stat().st_mtime_ns
            subprocess.run(["cmake", "-P", str(script)], check=True)
            self.assertEqual(out.stat().st_mtime_ns, before)
        self.assertEqual(SOURCE.read_bytes(), original_bytes)

    def test_dependency_drift_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "changed.cc"
            source.write_text(SOURCE.read_text() + "\n// upstream changed\n")
            out = Path(tmp) / "out.cc"
            script = Path(tmp) / "run.cmake"
            script.write_text(f'include("{PATCH.as_posix()}")\nstarter_patch_tflm_conv("{source.as_posix()}" "{out.as_posix()}")\n')
            result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("TFLM conv adapter changed", result.stderr)
            self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
