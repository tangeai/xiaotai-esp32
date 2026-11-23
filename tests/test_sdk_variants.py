"""Exercise the actual Python/CMake TiRTC package selection and contract checks."""
# Replaces legacy: tests/test_sdk_variants.py
import argparse
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
import xiaotai_build_native as native
import product_scope

SDK = ROOT / "components/tirtc_sdk"


def supported_lines(variant):
    return [line for line in build.sdk_lines(ROOT)["lines"]
            if native.profile_path(ROOT, variant["name"], line=line).is_file()]


def firmware_plan(variant, line=None):
    """The plan the build entry uses."""
    args = argparse.Namespace(idf=line, stage="firmware", fresh_config=False, write_lock=False,
                              component_index=None, component_cache=None)
    return native.native_plan(args, variant, {})[3]


class Variants(unittest.TestCase):
    def cmake(self, body):
        with tempfile.TemporaryDirectory() as folder:
            script = Path(folder) / "test.cmake"
            script.write_text(body, encoding="utf-8")
            return subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)

    def select(self, package):
        return f'''function(idf_build_get_property output property)
set(${{output}} "{package}" PARENT_SCOPE)
endfunction()
include("{(SDK / 'select_variant.cmake').as_posix()}")
'''

    @unittest.skipUnless(product_scope.has_product(product_scope.XMINI), product_scope.ABSENT)
    def test_xmini_sdk_uses_pinned_mini_with_active_pending_cancel(self):
        args = argparse.Namespace(idf=None, stage="firmware", fresh_config=False, write_lock=False,
                                  component_index=None, component_cache=None)
        _, package, contract, plan = native.native_plan(args, build.select("xmini/c3-v3"), {})
        self.assertEqual((contract["sdk_version"], contract["idf_version"]), ("2.5.0-mini-idf6-validation", "6.0.2"))
        self.assertEqual(hashlib.sha256((package["lib"] / "libTiRTC.a").read_bytes()).hexdigest(),
                         contract["sha256"])
        for name, expected in contract["headers"].items():
            self.assertEqual(hashlib.sha256((package["include"] / "tirtc" / name).read_bytes()).hexdigest(),
                             expected)
        lock = build.read_dependency_lock(ROOT / plan["dependency_lock"])
        self.assertEqual((lock["target"], lock["version"], lock["dependencies"]["idf"]["version"]),
                         ("esp32c3", "3.0.0", "6.0.2"))
        result = self.cmake(self.select(package["key"]) + f'''
include("{(ROOT / 'sdk/xiaotai_transport/esp-idf/verify_mini_contract.cmake').as_posix()}")
if(NOT tirtc_package_name STREQUAL "xmini-mini" OR
   NOT tirtc_contract_file STREQUAL "{package['contract'].as_posix()}")
    message(FATAL_ERROR "Python and CMake selected different Mini packages")
endif()
xiaotai_tirtc_verify_mini_contract("${{tirtc_contract_file}}" "${{tirtc_package_name}}"
    "{(ROOT / 'sdk/xiaotai_transport/esp-idf/include/tgtrp_internal_diag.h').as_posix()}" mini_pending_cancel)
if(NOT mini_pending_cancel)
    message(FATAL_ERROR "Pinned Mini must select the pending-cancel transport path")
endif()
''')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_each_line_selects_one_package_in_python_and_cmake(self):
        for variant in build.variants():
            for line in supported_lines(variant):
                with self.subTest(board=variant["board"], line=line):
                    package = build.sdk_package(variant, line)
                    contract = json.loads(package["contract"].read_text(encoding="utf-8"))
                    self.assertEqual(contract["target"], variant["target"])
                    self.assertEqual(hashlib.sha256((package["lib"] / "libTiRTC.a").read_bytes()).hexdigest(),
                                     contract["sha256"])
                    result = self.cmake(self.select(package["key"]) + f'''
if(NOT sdk_dir STREQUAL "{package['lib'].as_posix()}" OR
   NOT tirtc_contract_file STREQUAL "{package['contract'].as_posix()}")
message(FATAL_ERROR "Python/CMake SDK selector mismatch")
endif()
file(READ "${{tirtc_contract_file}}" contract)
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
''')
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_default_line_keeps_the_existing_environments(self):
        for variant in build.variants():
            for stage in ("firmware",):
                with self.subTest(board=variant["board"], stage=stage):
                    report = firmware_plan(variant)
                    own_line = native.load_profile(variant["name"])["idf_line"]
                    if own_line != "6.0":
                        # A product only a newer line supports builds on its own line by default.
                        self.assertEqual(report["idf_line"], own_line)
                        continue
                    xmini = variant["target"] == "esp32c3"
                    self.assertEqual(report["idf_line"], "6.0")
                    # Official TiRTC 2.6.3 packages pin ESP-IDF 6.0.3; Xmini keeps its 6.0.2 Mini build.
                    self.assertEqual(report["idf_environment"]["version"], "6.0.2" if xmini else "6.0.3")
                    self.assertEqual(report["tirtc_package"],
                                     "6.0/xmini-mini" if xmini else f"6.0/generic-{variant['target']}")
                    self.assertFalse(Path(report["build_dir"]).name.endswith("-idf5.5"))
                    if variant["target"] == "esp32s3":
                        for field in ("CONFIG_LIBC_PICOLIBC", "CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND"):
                            self.assertEqual(report["config"][field], "y")
                    elif variant["target"] == "esp32p4":
                        self.assertEqual(report["config"]["CONFIG_LIBC_NEWLIB"], "y")

    def test_idf55_line_uses_official_packages(self):
        supported = [v for v in build.variants() if "5.5" in supported_lines(v)]
        self.assertEqual(len(supported), 4)  # the four colour boards; Xmini has no 5.5 Mini package
        for variant in supported:
            with self.subTest(board=variant["board"]):
                report = firmware_plan(variant, "5.5")
                contract = json.loads(build.sdk_package(variant, "5.5")["contract"].read_text(encoding="utf-8"))
                self.assertEqual(contract["origin"], "official")
                self.assertEqual(report["idf_environment"]["version"], "5.5.5")
                self.assertEqual(report["tirtc_package"], f"5.5/generic-{variant['target']}")
                self.assertTrue(Path(report["build_dir"]).name.endswith("-idf5.5"))
                # The archive calls DES and keeps SHA-256 contexts; it was compiled against newlib.
                for field in ("CONFIG_MBEDTLS_DES_C", "CONFIG_MBEDTLS_HARDWARE_SHA", "CONFIG_LIBC_NEWLIB"):
                    self.assertEqual(report["config"][field], "y")
                self.assertNotIn("CONFIG_LIBC_PICOLIBC", report["config"])

    def test_unknown_or_missing_package_is_rejected(self):
        for package, message in (("", "No TiRTC package selected"), ("6.0/szpi-http", "Unknown TiRTC package"),
                                 ("9.9/generic-esp32s3", "Unknown TiRTC package")):
            result = self.cmake(self.select(package))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(message, result.stderr)

    def test_unsupported_line_is_reported_by_name(self):
        with self.assertRaisesRegex(ValueError, "Unknown ESP-IDF line 9.9"):
            firmware_plan(build.variants()[0], "9.9")
        # Xmini needs the pending-connection APIs of the 6.0 Mini package.
        if product_scope.has_product(product_scope.XMINI):
            with self.assertRaisesRegex(ValueError, "no locked native profile for xiaotai-xmini-c3-v3 on ESP-IDF 5.5"):
                firmware_plan(build.select("xmini/c3-v3"), "5.5")

    def crypto(self, mode, extra):
        contract = {"crypto_config": {"CONFIG_MBEDTLS_DES_C": "y"}}
        if mode:
            contract["crypto_mode"] = mode
        return self.cmake(f'''set(CONFIG_MBEDTLS_DES_C y)
set({extra} y)
include("{(SDK / 'crypto_contract.cmake').as_posix()}")
tirtc_validate_crypto_contract([==[{json.dumps(contract)}]==])
''')

    def test_full_crypto_contract_rejects_extra_settings_required_mode_allows_them(self):
        self.assertNotEqual(self.crypto(None, "CONFIG_MBEDTLS_AES_C").returncode, 0)
        self.assertNotEqual(self.crypto("full", "CONFIG_MBEDTLS_AES_C").returncode, 0)
        self.assertEqual(self.crypto("required", "CONFIG_MBEDTLS_AES_C").returncode, 0)
        self.assertNotEqual(self.crypto("other", "CONFIG_UNRELATED").returncode, 0)

    def capability(self, backend, symbols, expected):
        return self.cmake(f'''include("{(SDK / 'http_contract.cmake').as_posix()}")
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
                environment.verify(root, "9.9.9")

    def test_active_version_reads_the_tree(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.assertIsNone(environment.active_version(root))
            (root / "tools/cmake").mkdir(parents=True)
            (root / "tools/cmake/version.cmake").write_text(
                "set(IDF_VERSION_MAJOR 5)\nset(IDF_VERSION_MINOR 5)\nset(IDF_VERSION_PATCH 5)\n", encoding="utf-8")
            self.assertEqual(environment.active_version(root), "5.5.5")


if __name__ == "__main__":
    unittest.main()
