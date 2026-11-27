# Replaces legacy: tests/test_build_profiles.py
# Replaces legacy: tests/test_native_profiles.py
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
    def test_build_lock_left_by_an_ended_build_is_cleared(self):
        import subprocess
        child = subprocess.Popen([sys.executable, "-c", "pass"])
        child.wait()
        self.assertTrue(build.process_alive(os.getpid()))
        self.assertFalse(build.process_alive(child.pid))
        with tempfile.TemporaryDirectory() as temporary:
            lock = Path(temporary) / ".firmware-build.lock"
            me = build.build_owner()
            lock.write_text(json.dumps(dict(me, pid=child.pid)), encoding="utf-8")
            build.acquire_build_lock(lock)  # ended build on this machine: cleared and retaken
            self.assertEqual(json.loads(lock.read_text(encoding="utf-8"))["pid"], os.getpid())
            for owner in (dict(me, pid=os.getpid()),                     # still running
                          dict(me, pid=child.pid, tool_pid=os.getpid()),  # launcher gone, idf.py still building
                          dict(me, pid=child.pid, platform="other-os"),   # e.g. WSL vs Windows
                          dict(me, pid=child.pid, host="other-host"),
                          str(child.pid)):                                # old format: owner unknown
                lock.write_text(json.dumps(owner) if isinstance(owner, dict) else owner, encoding="utf-8")
                with self.subTest(owner=owner), self.assertRaisesRegex(ValueError, "Build already locked"):
                    build.acquire_build_lock(lock)
                self.assertTrue(lock.exists())

    def test_build_lock_is_released_only_by_its_owner(self):
        with tempfile.TemporaryDirectory() as temporary:
            lock = Path(temporary) / ".firmware-build.lock"
            lock.write_text(json.dumps(dict(build.build_owner(), pid=os.getpid() + 1)), encoding="utf-8")
            build.release_build_lock(lock)
            self.assertTrue(lock.exists())
            lock.write_text(json.dumps(build.build_owner()), encoding="utf-8")
            build.release_build_lock(lock)
            self.assertFalse(lock.exists())
            # A takeover in progress by another build is reported, not raced.
            child = __import__("subprocess").Popen([sys.executable, "-c", "pass"]); child.wait()
            lock.write_text(json.dumps(dict(build.build_owner(), pid=child.pid)), encoding="utf-8")
            guard = lock.with_name(lock.name + ".takeover"); guard.write_text("")
            with self.assertRaisesRegex(ValueError, "taking over"):
                build.acquire_build_lock(lock)
            guard.unlink()
            build.acquire_build_lock(lock)
            self.assertEqual(json.loads(lock.read_text(encoding="utf-8"))["pid"], os.getpid())

    def test_outdated_lock_fingerprint_is_detected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "dependencies.lock"
            path.write_text("dependencies: {}\nmanifest_hash: " + "a" * 64 + "\ntarget: esp32s3\n", encoding="utf-8")
            self.assertEqual(build.lock_fingerprint(path), "a" * 64)
            path.write_text("dependencies: {}\n", encoding="utf-8")
            self.assertIsNone(build.lock_fingerprint(path))

    def test_board_profiles(self):
        data = build.variants()
        import product_scope
        self.assertTrue(product_scope.expected({"xiaotai-szpi-s3", "xiaotai-cores3-lite", "xiaotai-waveshare-p4-rev3",
                                                "xiaotai-wt99p461-s1-1v1", "xiaotai-xmini-c3-v3"})
                        <= {v["name"] for v in data})
        targets = {"esp32s3", "esp32p4"} | ({"esp32c3"} if product_scope.has_product(product_scope.XMINI) else set())
        self.assertTrue(targets <= {v["target"] for v in data})
        self.assertFalse(any(v["xiaotai"]["hardware_tested"] for v in data if v.get("entry") != "sdk"))

    def test_unregistered_board_has_no_fallback(self):
        with self.assertRaises(ValueError):
            build.select("espressif/esp32-s3-box-3")

    def test_path_traversal_rejected(self):
        with self.assertRaises(ValueError):
            build.select("../../../../xiaotai", "../sdkconfig")

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

    def test_sdk_contracts_name_their_packages(self):
        """No TiRTC archive is stored in the tree; every downloaded package is named by hash,
        and its contract carries the origin its line records (official or local-build)."""
        import hashlib
        sdk = ROOT / "components/tirtc_sdk"
        self.assertFalse((sdk / "lib").exists() or (sdk / "idf5.5").exists())
        table = json.loads((sdk / "packages.json").read_text(encoding="utf-8"))
        headers = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (sdk / "include/tirtc").glob("*.h")}
        for line, spec in table["lines"].items():
            for name, entry in spec["packages"].items():
                if "path" not in entry:
                    continue
                contract = json.loads((sdk / entry["contract"]).read_text(encoding="utf-8"))
                self.assertIn(spec["origin"], ("official", "local-build"))
                self.assertEqual((contract["origin"], contract["sdk_version"], contract["package_sha256"]),
                                 (spec["origin"], entry["version"], entry["sha256"]))
                self.assertEqual(contract["target"], name.removeprefix("generic-"))
                self.assertIn(contract["idf_version"], spec["idf_versions"])
                self.assertEqual(contract["headers"], headers)


if __name__ == "__main__":
    unittest.main()
