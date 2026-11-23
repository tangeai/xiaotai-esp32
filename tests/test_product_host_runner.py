#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise product-run directory ownership without starting a compiler."""

from contextlib import ExitStack, redirect_stderr, redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET


spec = importlib.util.spec_from_file_location(
    "product_runner", Path(__file__).resolve().parent / "sdk/product/run.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
PRODUCT = "xiaotai-fixture"


@unittest.skipUnless(os.name == "posix", "product host runner requires POSIX")
class ProductHostRunnerTests(unittest.TestCase):
    def setUp(self):
        contexts = ExitStack()
        self.addCleanup(contexts.close)
        self.base = Path(contexts.enter_context(tempfile.TemporaryDirectory()))
        self.scratch = self.base / "scratch"
        self.state = self.base / "state"
        self.existing = self.scratch / "xiaotai-product-host-existing"
        self.existing.mkdir(parents=True)
        (self.existing / "keep").write_text("untouched", encoding="utf-8")
        contexts.enter_context(patch.object(tempfile, "tempdir", str(self.scratch)))
        contexts.enter_context(patch.dict(os.environ, {"XDG_STATE_HOME": str(self.state)}))
        contexts.enter_context(patch.object(runner.shutil, "which", side_effect=lambda program: program))
        contexts.enter_context(patch.object(runner.subprocess, "check_output",
            return_value=json.dumps({PRODUCT: {"default": {"at": False}}})))
        contexts.enter_context(patch.object(runner, "locked_sources", return_value=[]))
        contexts.enter_context(patch.object(runner, "step", side_effect=self.fake_step))
        self.failure_phase = None
        self.cache = None

    def fake_step(self, command, log, env):
        del env
        with log.open("a", encoding="utf-8") as stream:
            stream.write(f"{command[0]}: fixture\n")
        if self.failure_phase == "interrupt":
            raise KeyboardInterrupt
        if "-B" in command:
            self.cache = command[command.index("-B") + 1]
            self.cache.mkdir(exist_ok=True)
            if not (self.cache / "CMakeCache.txt").exists():
                (self.cache / "CMakeCache.txt").write_text("fixture cache", encoding="utf-8")
            if not (self.cache / "outside").is_symlink():
                (self.cache / "outside").symlink_to(self.existing, target_is_directory=True)
            return 2 if self.failure_phase == "configure" else 0
        if "--output-junit" in command:
            report = command[command.index("--output-junit") + 1]
            suite = ET.Element("testsuite")
            for name in sorted(runner.expected_tests(PRODUCT, "default", False, False)):
                case = ET.SubElement(suite, "testcase", name=name, status="run")
                if self.failure_phase == "test":
                    ET.SubElement(case, "failure")
            ET.ElementTree(suite).write(report, encoding="utf-8")
            return 1 if self.failure_phase == "test" else 0
        return 0

    def invoke(self, *arguments):
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            result = runner.main([*map(str, arguments), "--parallel-products", "1", "-j", "1"])
        paths = [Path(line.removeprefix("Evidence: ")) for line in stdout.getvalue().splitlines()
                 if line.startswith("Evidence: ")]
        self.assertLessEqual(len(paths), 1, stdout.getvalue())
        return result, paths[0] if paths else None, stderr.getvalue()

    def assert_scratch_clean(self):
        self.assertEqual(list(self.scratch.iterdir()), [self.existing])
        self.assertEqual((self.existing / "keep").read_text(encoding="utf-8"), "untouched")

    def test_ledger_registrations_follow_product_and_case(self):
        for product_id in ("szpi-s3", "cores3-lite", "xmini-c3-v3", "waveshare-p4-rev3", "wt99p461-s1-1v1"):
            prefix = f"product_{product_id.replace('-', '_')}_"
            for case in ("default", "variant-no-video", "without-room"):
                for boot_only in (False, True):
                    with self.subTest(product=product_id, case=case, boot_only=boot_only):
                        expected = {"start_owner_fault"} if case == "default" else set()
                        if case == "default" and not boot_only and product_id in ("szpi-s3", "cores3-lite"):
                            expected.update(f"profile_video_model_{probe}" for probe in (
                                "null", "empty", "quote", "backslash", "newline", "space", "forward_slash",
                                "bytes63", "bytes64",
                            ))
                        names = runner.expected_tests(f"xiaotai-{product_id}", case, boot_only, False)
                        registrations = {name.removeprefix(prefix) for name in names}
                        self.assertEqual({name for name in registrations if name == "start_owner_fault" or
                                          name.startswith("profile_video_model_")}, expected)

    def test_default_cleans_only_own_cache_and_keeps_evidence(self):
        result, evidence, _ = self.invoke()
        self.assertEqual(result, 0)
        self.assert_scratch_clean()
        self.assertEqual(evidence.parent, self.state / "xiaotai" / "product-host")
        self.assertTrue((evidence / PRODUCT / "default.log").is_file())
        self.assertTrue((evidence / PRODUCT / "default.xml").is_file())

    def test_configure_and_test_failure_cleanup(self):
        for phase in ("configure", "test"):
            with self.subTest(phase=phase):
                self.failure_phase = phase
                result, evidence, _ = self.invoke("--evidence-root", self.base / "failure")
                self.assertEqual(result, 1)
                self.assertEqual(evidence.parent, self.base / "failure")
                self.assertTrue((evidence / PRODUCT / "default.log").is_file())
                self.assertEqual((evidence / PRODUCT / "default.xml").exists(), phase == "test")
                self.assert_scratch_clean()

    def test_evidence_setup_error_still_cleans_own_cache(self):
        blocked = self.base / "not-a-directory"
        blocked.write_text("keep", encoding="utf-8")
        result, evidence, error = self.invoke("--evidence-root", blocked)
        self.assertEqual(result, 1)
        self.assertIsNone(evidence)
        self.assertIn("product evidence directory", error)
        self.assertIsNone(self.cache)
        self.assertEqual(blocked.read_text(encoding="utf-8"), "keep")
        self.assert_scratch_clean()

    def test_interrupted_matrix_keeps_partial_evidence(self):
        self.failure_phase = "interrupt"
        parent = self.base / "interrupted"
        with self.assertRaises(KeyboardInterrupt):
            self.invoke("--evidence-root", parent)
        self.assertEqual(len(list(parent.glob("evidence-*/xiaotai-fixture/default.log"))), 1)
        self.assert_scratch_clean()

    def test_explicit_build_root_reuses_cache_and_keeps_each_report(self):
        explicit = self.base / "kept"
        result, first, _ = self.invoke("--build-root", explicit)
        self.assertEqual(result, 0)
        cache = explicit / PRODUCT / "CMakeCache.txt"
        cache.write_text("previous invocation", encoding="utf-8")
        result, second, _ = self.invoke("--build-root", explicit)
        self.assertEqual(result, 0)
        self.assertEqual(cache.read_text(encoding="utf-8"), "previous invocation")
        self.assertNotEqual(first, second)
        for evidence in (first, second):
            self.assertEqual(evidence.parent, explicit)
            self.assertTrue((evidence / PRODUCT / "default.xml").is_file())
        self.failure_phase = "test"
        result, failed, _ = self.invoke("--build-root", explicit)
        self.assertEqual(result, 1)
        self.assertEqual(cache.read_text(encoding="utf-8"), "previous invocation")
        self.assertTrue((failed / PRODUCT / "default.xml").is_file())
        self.assert_scratch_clean()

    def test_lock_conflict_does_not_create_evidence(self):
        import fcntl
        explicit = self.base / "busy"
        explicit.mkdir()
        with (explicit / ".matrix.lock").open("w", encoding="utf-8") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result, evidence, error = self.invoke("--build-root", explicit)
        self.assertEqual(result, 1)
        self.assertIsNone(evidence)
        self.assertIn("build root is in use", error)
        self.assertEqual(list(explicit.glob("evidence-*")), [])
        self.assert_scratch_clean()


if __name__ == "__main__":
    unittest.main()
