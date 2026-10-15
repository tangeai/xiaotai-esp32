"""Exercise release guards with synthetic inputs, never with attached hardware."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
import zipfile
from unittest.mock import patch
from urllib.parse import unquote, urlsplit

from package_release import digest, package, verify_full_image


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.candidate = self.root / "candidate"
        self.source = self.candidate / "source"
        self.build = self.source / "build"
        self.output = self.root / "output"
        self.header = self.root / "sdkconfig.h"
        self.header.write_text("#define TEST_CONFIG 1\n")
        self.put("main/app.c", "int main(void) { return 0; }\n")
        self.put("partitions.csv", "nvs,data,nvs,0x9000,0x6000\nfactory,app,factory,0x10000,0x1f0000\n")
        self.put("docs/FLASHING.md", "xiaotai-mz28-0.1.0-pre-release-full-8MB.bin\n"
                 "tirtc_c3_wechat_voip.bin\n")
        for name in ("LICENSE.txt", "docs/UPSTREAM_LICENSE.txt", "docs/TBOX_LICENSE.txt",
                     "docs/ESP_AUDIO_CODEC_LICENSE.txt", "docs/ESP_IDF_LICENSE.txt"):
            self.put(name, "Synthetic license\n")
        files = {p.relative_to(self.source).as_posix(): digest(p)
                 for p in self.source.rglob("*") if p.is_file()}
        self.manifest = {"version": "0.1.0", "status": "pre-release", "files": files, "source_commit": None,
                         "project_name": "xiaotai_mz28_ai", "artifact_name": "xiaotai-mz28",
                         "flash_size": "8MB"}
        self.write_json(self.candidate / "source-manifest.json", self.manifest)
        (self.candidate / "VALIDATION.md").write_text("# Synthetic validation\n")
        self.put("build/config/sdkconfig.h", self.header.read_text())
        self.put("build/bootloader/bootloader.bin", "boot")
        self.put("build/partition_table/partition-table.bin", "partition")
        self.put("build/tirtc_c3_wechat_voip.bin", "application")
        self.put("build/tirtc_c3_wechat_voip.elf", "elf")
        self.description = {"project_name": "xiaotai_mz28_ai", "project_version": "0.1.0", "target": "esp32c3",
                            "git_revision": "v5.5.5", "project_path": str(self.source),
                            "app_bin": "tirtc_c3_wechat_voip.bin", "app_elf": "tirtc_c3_wechat_voip.elf"}
        self.write_json(self.build / "project_description.json", self.description)
        self.flash = {"extra_esptool_args": {"chip": "esp32c3"},
                      "flash_settings": {"flash_mode": "dio", "flash_size": "8MB", "flash_freq": "80m"},
                      "write_flash_args": ["--flash_mode", "dio", "--flash_size", "8MB", "--flash_freq", "80m"],
                      "flash_files": {"0x0": "bootloader/bootloader.bin", "0x8000": "partition_table/partition-table.bin",
                                      "0x10000": "tirtc_c3_wechat_voip.bin"}}
        self.write_json(self.build / "flasher_args.json", self.flash)
        self.merge = patch("package_release.subprocess.run", side_effect=self.fake_merge).start()
        self.addCleanup(patch.stopall)

    def fake_merge(self, command, check):
        self.assertTrue(check)
        self.assertIn("merge_bin", command)
        self.assertEqual(command[command.index("--fill-flash-size") + 1], "8MB")
        for option in ("--flash_mode", "--flash_freq", "--flash_size"):
            self.assertEqual(command[command.index(option) + 1], "keep")
        data = bytearray(b"\xff" * (8 * 1024 * 1024))
        start = command.index("--fill-flash-size") + 2
        for index in range(start, len(command), 2):
            offset = int(command[index], 0)
            content = Path(command[index + 1]).read_bytes()
            data[offset:offset + len(content)] = content
        Path(command[command.index("--output") + 1]).write_bytes(data)

    def put(self, name, data):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(data)

    def write_json(self, path, data):
        path.write_text(json.dumps(data), encoding="utf-8")

    def run_package(self):
        return package(self.candidate, self.output, self.header)

    def test_valid_package(self):
        self.run_package()
        with zipfile.ZipFile(next(self.output.glob("*-source.zip"))) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(len(archive.namelist()), 8)
            self.assertFalse(any("build/" in name for name in archive.namelist()))
        for line in (self.output / "SHA256SUMS.txt").read_text().splitlines():
            expected, name = line.split("  ", 1)
            self.assertEqual(digest(self.output / name), expected)
        manifest = json.loads((self.output / "release-manifest.json").read_text())
        self.assertEqual([s["offset"] for s in manifest["flash"]], ["0x0", "0x8000", "0x10000"])
        self.assertEqual(manifest["validation"]["sha256"], digest(self.output / "VALIDATION.md"))
        full_image = self.output / manifest["full_image"]["file"]
        self.assertEqual(full_image.stat().st_size, 8 * 1024 * 1024)
        self.assertEqual(digest(full_image), manifest["full_image"]["sha256"])
        self.assertFalse(manifest["full_image"]["preserves_existing_nvs"])
        self.assertEqual(full_image.read_bytes()[0x9000:0xf000], b"\xff" * 0x6000)
        with zipfile.ZipFile(self.output / manifest["firmware_archive"]["file"]) as archive:
            self.assertIsNone(archive.testzip())
            prefix = "xiaotai-mz28-0.1.0-pre-release-firmware/"
            self.assertIn(prefix + "TBOX_LICENSE.txt", archive.namelist())
            self.assertIn(prefix + "ESP_AUDIO_CODEC_LICENSE.txt", archive.namelist())
            for line in archive.read(prefix + "SHA256SUMS.txt").decode().splitlines():
                expected, name = line.split("  ", 1)
                self.assertEqual(hashlib.sha256(archive.read(prefix + name)).hexdigest(), expected)

    def test_merge_failure_blocks_package(self):
        self.merge.side_effect = subprocess.CalledProcessError(1, ["esptool"])
        with self.assertRaisesRegex(ValueError, "merge failed"):
            self.run_package()
        self.assertFalse((self.output / "release-manifest.json").exists())

    def test_stable_release_package_names(self):
        self.manifest.update(version="1.0.0", status="release")
        self.put("docs/FLASHING.md", "xiaotai-mz28-1.0.0-full-8MB.bin\n"
                 "tirtc_c3_wechat_voip.bin\n")
        self.manifest["files"]["docs/FLASHING.md"] = digest(self.source / "docs/FLASHING.md")
        self.write_json(self.candidate / "source-manifest.json", self.manifest)
        self.description["project_version"] = "1.0.0"
        self.write_json(self.build / "project_description.json", self.description)
        self.run_package()
        self.assertTrue((self.output / "xiaotai-mz28-1.0.0-source.zip").is_file())
        self.assertTrue((self.output / "xiaotai-mz28-1.0.0-firmware.zip").is_file())
        manifest = json.loads((self.output / "release-manifest.json").read_text())
        self.assertEqual(manifest["status"], "release")
        self.assertEqual(manifest["build"]["app_version"], "1.0.0")
        self.assertEqual(manifest["full_image"]["file"], "firmware/xiaotai-mz28-1.0.0-full-8MB.bin")
        for archive_path in self.output.glob("*.zip"):
            with zipfile.ZipFile(archive_path) as archive:
                self.assertIsNone(archive.testzip())
                self.assertFalse(any("pre-release" in name for name in archive.namelist()))

    def test_unsupported_manifest_status_rejected(self):
        self.manifest["status"] = "unknown"
        self.write_json(self.candidate / "source-manifest.json", self.manifest)
        with self.assertRaisesRegex(ValueError, "release or pre-release manifest"):
            self.run_package()
        self.assertFalse(self.output.exists())

    def test_unsupported_profile_status_rejected(self):
        from prepare_release import prepare
        root = self.root / "invalid-profile"
        (root / "config").mkdir(parents=True)
        self.write_json(root / "config/release-profile.json", {
            "project_name": "xiaotai_mz28_ai", "flash_size": "8MB", "status": "unknown",
        })
        with self.assertRaisesRegex(ValueError, "release or pre-release profile"):
            prepare(root, self.output, self.header, self.header)
        self.assertFalse(self.output.exists())

    def test_corrupt_merged_segment_blocks_package(self):
        def corrupt(command, check):
            self.fake_merge(command, check)
            path = Path(command[command.index("--output") + 1])
            with path.open("r+b") as stream:
                stream.seek(0x10000)
                stream.write(b"!")
        self.merge.side_effect = corrupt
        with self.assertRaisesRegex(ValueError, "differs from"):
            self.run_package()

    def test_gap_data_rejected(self):
        path = self.root / "full.bin"
        path.write_bytes(b"boot\xffN\xffapplication\xff")
        with self.assertRaisesRegex(ValueError, "gap data"):
            verify_full_image(path, [(0, self.build / "bootloader/bootloader.bin"),
                                     (7, self.build / "tirtc_c3_wechat_voip.bin")], 19)

    def test_tail_data_rejected(self):
        path = self.root / "full.bin"
        path.write_bytes(b"boot\xff\xffN\xff")
        with self.assertRaisesRegex(ValueError, "tail data"):
            verify_full_image(path, [(0, self.build / "bootloader/bootloader.bin")], 8)

    def test_incorrect_full_image_size_rejected(self):
        path = self.root / "full.bin"
        path.write_bytes(b"boot")
        with self.assertRaisesRegex(ValueError, "physical flash size"):
            verify_full_image(path, [], 8)

    def test_wrong_manifest_rejected(self):
        self.manifest["flash_size"] = "2MB"
        self.write_json(self.candidate / "source-manifest.json", self.manifest)
        with self.assertRaisesRegex(ValueError, "MZ28 8MB"):
            self.run_package()

    def test_no_overwrite(self):
        self.output.mkdir()
        with self.assertRaisesRegex(ValueError, "Output exists"):
            self.run_package()

    def test_changed_source(self):
        self.put("main/app.c", "changed")
        with self.assertRaisesRegex(ValueError, "Source changed"):
            self.run_package()
        self.assertFalse(self.output.exists())

    def test_private_override(self):
        self.put("main/app_config.local.h", "private")
        with self.assertRaisesRegex(ValueError, "Private"):
            self.run_package()

    def test_unfrozen_input(self):
        self.put("main/extra.c", "extra")
        with self.assertRaisesRegex(ValueError, "Unfrozen"):
            self.run_package()

    def test_header_mismatch(self):
        self.header.write_text("different")
        with self.assertRaisesRegex(ValueError, "headers differ"):
            self.run_package()

    def test_wrong_idf(self):
        self.description["git_revision"] = "v5.5.4"
        self.write_json(self.build / "project_description.json", self.description)
        with self.assertRaisesRegex(ValueError, "Build does not identify"):
            self.run_package()

    def test_segment_overlap(self):
        self.put("build/bootloader/bootloader.bin", "x" * 0x8001)
        with self.assertRaisesRegex(ValueError, "overlaps"):
            self.run_package()

    def test_flash_path_escape(self):
        self.flash["flash_files"]["0x10000"] = "../main/app.c"
        self.write_json(self.build / "flasher_args.json", self.flash)
        with self.assertRaisesRegex(ValueError, "Invalid input"):
            self.run_package()

    def test_wrong_flash_settings(self):
        self.flash["flash_settings"]["flash_size"] = "2MB"
        self.write_json(self.build / "flasher_args.json", self.flash)
        with self.assertRaisesRegex(ValueError, "flash settings"):
            self.run_package()

    def test_validation_required(self):
        (self.candidate / "VALIDATION.md").unlink()
        with self.assertRaisesRegex(ValueError, "validation report"):
            self.run_package()

    def test_modified_managed_dependency_blocks_packaging(self):
        with patch("package_release.validate_managed", side_effect=ValueError("Dependency hash mismatch")):
            with self.assertRaisesRegex(ValueError, "Dependency hash mismatch"):
                self.run_package()
        self.assertFalse(self.output.exists())

    def test_preparation_keeps_portal_and_dependency_inputs(self):
        from prepare_release import inputs
        root = Path(__file__).resolve().parents[1]
        selected = {p.as_posix() for p in inputs(root)}
        for name in ("components/wifi_provisioning/web/setup.html",
                     "components/product_audio/idf_component.yml", "dependencies.lock",
                     "components/product_audio/assets/prompts/wifi_provisioning.opuspkt",
                     "docs/USER_GUIDE.md", "docs/FLASHING.md", "docs/GETTING_STARTED_CN.md",
                     "docs/TESTING.md", "docs/README.md", "CHANGELOG.md", "README.md", "LICENSE.txt"):
            self.assertIn(name, selected)
        self.assertFalse(any(".local." in name or "build/" in name for name in selected))

    def test_public_document_links_are_exported(self):
        from prepare_release import inputs
        root = Path(__file__).resolve().parents[1]
        selected = {p.as_posix() for p in inputs(root)}
        for name in sorted(selected):
            if not name.endswith(".md"):
                continue
            path = root / name
            for target in re.findall(r"\]\(([^)]+)\)", path.read_text(encoding="utf-8")):
                parts = urlsplit(target.strip("<>"))
                if parts.scheme:
                    continue
                destination = (path.parent / unquote(parts.path)).resolve() if parts.path else path
                self.assertTrue(destination.is_relative_to(root), (name, target))
                self.assertIn(destination.relative_to(root).as_posix(), selected, (name, target))
                self.assertTrue(destination.is_file(), (name, target))
                if parts.fragment:
                    headings = re.findall(r"^#+ (.+)$", destination.read_text(encoding="utf-8"), re.M)
                    anchors = {re.sub(r"[^\w\-\u4e00-\u9fff]", "", h.lower().replace(" ", "-")) for h in headings}
                    self.assertIn(unquote(parts.fragment), anchors, (name, target))

    def test_preparation_exports_complete_device_guides(self):
        from prepare_release import inputs, prepare
        root = Path(__file__).resolve().parents[1]
        export = prepare(root, self.root / "prepared", root / "components/tirtc_sdk/lib/libTiRTC.a",
                         root / "sdkconfig")
        manifest = json.loads((export.parent / "source-manifest.json").read_text(encoding="utf-8"))
        expected = {p.as_posix() for p in inputs(root)} | {"sdkconfig", "docs/UPSTREAM_LICENSE.txt"}
        self.assertEqual(set(manifest["files"]), expected)
        for name in expected:
            self.assertEqual(digest(export / name), digest(root / name), name)
        self.assertFalse((export / ".local").exists())
        self.assertFalse((export / ".git").exists())
        self.assertFalse((export / "docs/ARCHITECTURE.md").exists())


if __name__ == "__main__":
    unittest.main()
