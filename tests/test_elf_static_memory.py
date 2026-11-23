"""Static memory records compare linked app artifacts, not intermediate builds."""

import hashlib
from importlib.util import find_spec
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import elf_static_memory as memory


class Section(dict):
    def __init__(self, name, size, flags=2):
        super().__init__(sh_size=size, sh_flags=flags)
        self.name = name


class StaticMemoryTests(unittest.TestCase):
    def setUp(self):
        directory = TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.out = Path(directory.name) / "sdk-firmware"
        self.out.mkdir()
        self.record = {"name": "fixture-product", "board": "fixture-board", "target": "esp32s3",
                       "entry": "sdk", "stage": "firmware",
                       "idf_identity": {"version": "6.0.3", "source_tree_sha256": "source"},
                       "artifacts": {"app.elf": "sha"}}

    def report(self, sizes=None, *, record=None, config=None, stamp=10):
        sections = dict.fromkeys(memory.CORE_SECTIONS, 4000)
        sections.update(sizes or {})
        with patch.object(memory, "read_app_elf", return_value=("app.elf", "sha", "EM_XTENSA", sections)):
            return memory.summarize(self.out, record or self.record, config or {}, completed_at_ns=stamp)

    def archive(self, label, data):
        path = self.out / "build-history" / f"attempt-{label}" / "build-record.json"
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def test_sections_are_selected_by_role_not_segment_ranges(self):
        sections = [Section(".iram0.text", 50), Section(".dram0.bss", 80), Section(".ext_ram.bss", 100),
                    Section(".dram0.dummy", 2048), Section(".ext_ram.dummy", 4096),
                    Section(".flash_rodata_dummy", 8192), Section(".flash.rodata", 40),
                    Section(".debug_info", 104, 0), Section(".ext_ram.noinit", 1, 0),
                    Section(".spm.data", 72), Section(".rtc.data", 9)]
        self.assertEqual(memory.section_sizes(sections), {".iram0.text": 50, ".dram0.data": 0,
                         ".dram0.bss": 80, ".ext_ram.bss": 100, ".flash.rodata": 40,
                         ".spm.data": 72, ".rtc.data": 9})

    def test_psram_xip_separate_from_ram_static_sections(self):
        report = self.report({".flash.text": 9000, ".flash.rodata": 8000, ".ext_ram.bss": 7000},
                             config={"CONFIG_SPIRAM_FETCH_INSTRUCTIONS": "y", "CONFIG_SPIRAM_RODATA": "n"})
        self.assertEqual(report["psram_xip_section_bytes"], {".flash.text": 9000})
        self.assertNotIn(".flash.text", report["section_bytes"])
        self.assertNotIn(".flash.rodata", report["section_bytes"])
        self.assertEqual(report["section_bytes"][".ext_ram.bss"], 7000)

    def test_latest_compatible_success_and_strict_threshold(self):
        first = self.report(stamp=4)
        self.assertNotIn("baseline", first)
        self.archive("zzz", {**self.record, "static_memory": first})
        latest = self.report({".ext_ram.bss": 2000}, stamp=5)
        path = self.archive("abc", {**self.record, "static_memory": latest})
        self.archive("legacy-record", self.record)
        self.archive("missing-artifact", {**self.record, "artifacts": {}, "static_memory": {**latest,
                     "completed_at_ns": 6}})
        self.archive("invalid-record", None)
        report = self.report({".iram0.text": 5024, ".dram0.data": 2975, ".dram0.bss": 5025}, stamp=7)
        self.assertEqual(report["baseline"]["record"], path.relative_to(self.out).as_posix())
        self.assertEqual(report["changes_over_1kib"], {
            ".dram0.data": {"before": 4000, "after": 2975, "delta": -1025},
            ".dram0.bss": {"before": 4000, "after": 5025, "delta": 1025},
            ".ext_ram.bss": {"before": 2000, "after": 0, "delta": -2000},
        })

    def test_never_compares_other_entry_target_product_idf_or_accounting(self):
        first = self.report(stamp=4)
        self.archive("first", {**self.record, "static_memory": first})
        for key, value in {"name": "elsewhere", "board": "other-board", "target": "esp32p4",
                           "entry": "legacy", "stage": "sdk-check", "idf_identity": {"version": "5.5.5"}}.items():
            with self.subTest(key=key):
                self.assertNotIn("baseline", self.report(record={**self.record, key: value}))
        self.assertNotIn("baseline", self.report(config={"CONFIG_SPIRAM_RODATA": "y"}))

    @unittest.skipUnless(find_spec("elftools"), "requires the existing ESP-IDF interpreter")
    def test_missing_or_mismatched_app_artifact_never_reports_zero_as_measurement(self):
        description = self.out / "project_description.json"
        record = {**self.record, "artifacts": {"app.elf": hashlib.sha256(b"original").hexdigest(),
                                              "bootloader.elf": "unrelated"}}
        (self.out / "app.elf").write_bytes(b"changed")
        for name, target, reason in [("../app.elf", "esp32s3", "identify"),
                                     ("app.elf", "esp32c3", "identify"),
                                     ("app.elf", "esp32s3", "changed after artifact hashing")]:
            with self.subTest(name=name, target=target):
                description.write_text(json.dumps({"app_elf": name, "target": target}), encoding="utf-8")
                report = memory.summarize(self.out, record, {}, completed_at_ns=10)
                self.assertEqual(report["status"], "unavailable")
                self.assertIn(reason, report["reason"])
                self.assertNotIn("section_bytes", report)


if __name__ == "__main__":
    unittest.main()
