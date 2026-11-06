"""Exercise the actual Python/CMake selectors and HTTP capability checks."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import idf_environment as environment
import xiaotai_build as build


class Variants(unittest.TestCase):
    def cmake(self, body):
        with tempfile.TemporaryDirectory() as folder:
            script = Path(folder) / "test.cmake"
            script.write_text(body, encoding="utf-8")
            return subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)

    def test_each_selector_matches_actual_library_and_headers(self):
        for variant in build.variants():
            with self.subTest(board=variant["board"]):
                directory = build.sdk_directory(variant)
                contract = json.loads((directory / "contract.json").read_text(encoding="utf-8"))
                self.assertEqual(hashlib.sha256((directory / "libTiRTC.a").read_bytes()).hexdigest(), contract["sha256"])
                code = f'''function(idf_build_get_property output property)
set(${{output}} "{variant['board']}" PARENT_SCOPE)
endfunction()
set(IDF_TARGET "{variant['target']}")
include("{(ROOT / 'components/tirtc_sdk/select_variant.cmake').as_posix()}")
if(NOT sdk_dir STREQUAL "{directory.as_posix()}")
message(FATAL_ERROR "Python/CMake SDK selector mismatch")
endif()
file(READ "${{sdk_dir}}/contract.json" contract)
string(JSON count ERROR_VARIABLE error LENGTH "${{contract}}" headers)
if(NOT error)
math(EXPR last "${{count}} - 1")
foreach(i RANGE ${{last}})
string(JSON name MEMBER "${{contract}}" headers ${{i}})
string(JSON expected GET "${{contract}}" headers "${{name}}")
file(SHA256 "${{tirtc_include_dir}}/tirtc/${{name}}" actual)
if(NOT expected STREQUAL actual)
message(FATAL_ERROR "Mixed SDK headers")
endif()
endforeach()
endif()
'''
                result = self.cmake(code)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_only_szpi_selects_new_environment(self):
        for variant in build.variants():
            for stage in ("firmware", "sdk-check"):
                with self.subTest(board=variant["board"], stage=stage):
                    report = build.plan(variant, stage)
                    is_szpi = variant["board"] == "lckfb/szpi-esp32s3"
                    self.assertEqual(report["idf_environment"]["version"], "6.0.3" if is_szpi else "6.0.2")
                    self.assertEqual(report["compatibility"]["sdk_variant"] == "szpi-http", is_szpi)
                    if is_szpi:
                        self.assertEqual(report["config"]["CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY"], "n")
                        for field in ("CONFIG_LIBC_PICOLIBC", "CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY",
                                      "CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND"):
                            self.assertEqual(report["config"][field], "y")

    def test_wrong_chip_cannot_select_szpi(self):
        result = self.cmake(f'''function(idf_build_get_property output property)
set(${{output}} lckfb/szpi-esp32s3 PARENT_SCOPE)
endfunction()
set(IDF_TARGET esp32c3)
include("{(ROOT / 'components/tirtc_sdk/select_variant.cmake').as_posix()}")
''')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ESP32-S3-only", result.stderr)

    def capability(self, backend, symbols, expected):
        return self.cmake(f'''include("{(ROOT / 'components/tirtc_sdk/http_contract.cmake').as_posix()}")
tirtc_http_capability([==[{json.dumps({'http_tls_backend': backend})}]==] [==[{symbols}]==] actual)
if(NOT actual STREQUAL "{expected}")
message(FATAL_ERROR "Incorrect HTTP capability")
endif()
''')

    def test_plain_http_is_explicit(self):
        self.assertEqual(self.capability("none", "lib: httpclt.o: U socket\n", 0).returncode, 0)

    def test_https_requires_all_four_symbols(self):
        symbols = [f"lib:httpclt.o: U mbedtls_ssl_{name}\n" for name in ("handshake", "setup", "read", "write")]
        self.assertEqual(self.capability("mbedtls", "".join(symbols), 1).returncode, 0)
        for missing in range(4):
            self.assertNotEqual(self.capability("mbedtls", "".join(symbols[:missing] + symbols[missing + 1:]), 1).returncode, 0)

    def test_capability_cannot_hide_archive_mismatch(self):
        for backend, symbols in (("none", ""), ("none", "lib:httpclt.o: U mbedtls_ssl_setup\n"),
                                 ("unknown", "lib:httpclt.o: U socket\n"),
                                 ("mbedtls", "lib:other.o: U mbedtls_ssl_handshake\n")):
            self.assertNotEqual(self.capability(backend, symbols, 0).returncode, 0)


class Environment(unittest.TestCase):
    def test_source_identity_covers_contents_paths_and_additions(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in ("CMakeLists.txt", "Kconfig", "sdkconfig.rename", "tools/idf.py", "components/a.c"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name, encoding="utf-8")
            initial = environment.source_identity(root)
            file = root / "components/a.c"
            file.write_text("changed", encoding="utf-8")
            self.assertNotEqual(environment.source_identity(root), initial)
            file.write_text("components/a.c", encoding="utf-8")
            self.assertEqual(environment.source_identity(root), initial)
            file.rename(root / "components/b.c")
            self.assertNotEqual(environment.source_identity(root), initial)
            (root / "components/b.c").rename(file)
            (root / "tools/extra.py").write_text("extra", encoding="utf-8")
            self.assertNotEqual(environment.source_identity(root), initial)
            (root / "tools/extra.py").unlink()
            cache = root / "tools/__pycache__"
            cache.mkdir()
            (cache / "generated.pyc").write_bytes(b"generated")
            self.assertEqual(environment.source_identity(root), initial)
            with self.assertRaisesRegex(ValueError, "differs"):
                environment.verify(root, "6.0.3")
            with self.assertRaisesRegex(ValueError, "No validated"):
                environment.verify(root, "6.1.0")


if __name__ == "__main__":
    unittest.main()
