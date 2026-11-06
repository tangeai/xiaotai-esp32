"""A later failed build cannot masquerade as the last successful attempt."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("xiaotai_build", ROOT / "scripts/xiaotai_build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildHistoryTests(unittest.TestCase):
    def test_ninja_only_input_with_spaces_is_recorded(self):
        with tempfile.TemporaryDirectory(prefix="input's inventory ") as directory:
            root = Path(directory)
            (root / "source.c").write_text("int value(void) { return 7; }\n")
            payload = root / "payload data.bin"
            payload.write_bytes(b"resource input")
            (root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(input_inventory C)
add_library(fixture OBJECT source.c)
add_custom_command(OUTPUT copied.bin
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/payload data.bin" copied.bin
  DEPENDS "${CMAKE_SOURCE_DIR}/payload data.bin" VERBATIM)
add_custom_target(resource ALL DEPENDS copied.bin)
''')
            out = root / "out"
            build.request_cmake_inputs(out)
            subprocess.run(["cmake", "-G", "Ninja", "-S", str(root), "-B", str(out)], check=True)
            subprocess.run(["cmake", "--build", str(out)], check=True)
            self.assertIn(payload.resolve(), build.build_input_paths(out))

    def test_inventory_includes_configure_inputs_and_compiler_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "value.h").write_text("#define VALUE 7\n")
            (root / "source.c").write_text('#include "value.h"\nint value(void) { return VALUE; }\n')
            (root / "source_meta").mkdir()
            (root / "generator.cmake").write_text('set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/adapter patch.patch" "${CMAKE_SOURCE_DIR}/source_meta")\n')
            (root / "adapter patch.patch").write_text("patch input\n")
            (root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(input_inventory C)
include(generator.cmake)
add_library(fixture OBJECT source.c)
''')
            out = root / "out"
            build.request_cmake_inputs(out)
            subprocess.run(["cmake", "-G", "Ninja", "-S", str(root), "-B", str(out)], check=True)
            subprocess.run(["cmake", "--build", str(out)], check=True)
            paths = build.build_input_paths(out)
            for name in ("source.c", "value.h", "CMakeLists.txt", "generator.cmake", "adapter patch.patch"):
                self.assertIn((root / name).resolve(), paths)
            self.assertNotIn(root / "source_meta", paths)
            (root / "adapter patch.patch").unlink()
            with self.assertRaisesRegex(ValueError, "CMake input disappeared"):
                build.cmake_input_paths(out)

    def test_missing_cmake_inventory_is_not_ignored(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "CMake input inventory is missing"):
                build.cmake_input_paths(Path(directory))

    def test_first_attempt_has_no_history(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertIsNone(build.archive_build_record(root))
            self.assertFalse((root / "build-history").exists())

    def test_failed_attempt_preserves_evidence_but_not_current_success(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "build-record.json").write_bytes(b'{"passed":true}')
            (root / "build-inputs.json").write_bytes(b'{"source":"hash"}')
            (root / "firmware.bin").write_bytes(b"artifact")
            first = build.archive_build_record(root)
            self.assertFalse((root / "build-record.json").exists())
            self.assertFalse((root / "build-inputs.json").exists())
            self.assertEqual((first / "build-record.json").read_bytes(), b'{"passed":true}')
            self.assertEqual((first / "build-inputs.json").read_bytes(), b'{"source":"hash"}')
            self.assertEqual((root / "firmware.bin").read_bytes(), b"artifact")
            (root / "build-record.json").write_bytes(b"next")
            second = build.archive_build_record(root)
            self.assertNotEqual(first, second)
            self.assertEqual((second / "build-record.json").read_bytes(), b"next")


if __name__ == "__main__":
    unittest.main()
