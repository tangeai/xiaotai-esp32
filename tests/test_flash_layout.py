"""Exercise generated flash layout identity without opening a serial port."""
# Replaces legacy: tests/test_flash_layout.py
import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from verify_build import sha256, verify
from flash_layout import flash_layout
from xiaotai_build_native import load_profile


class FlashLayoutTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.out = Path(work.name).resolve()
        self.args_path = self.out / "flasher_args.json"
        self.args = {
            "extra_esptool_args": {"chip": "esp32p4"},
            "flash_settings": {"flash_size": "16MB"},
            "flash_files": {
                "0x2000": "bootloader/bootloader.bin",
                "0x8000": "partition_table/partition-table.bin",
                "0xd000": "ota_data_initial.bin",
                "0x10000": "xiaotai.bin",
            },
        }
        for name, offset in (("bootloader", "0x2000"), ("partition-table", "0x8000"), ("app", "0x10000")):
            self.args[name] = {"offset": offset, "file": self.args["flash_files"][offset]}
        for name in self.args["flash_files"].values():
            file = self.out / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(name.encode("ascii"))
        self.write_p4_image("bootloader/bootloader.bin")
        self.write_p4_image("xiaotai.bin")
        self.write_args()

    def write_p4_image(self, name, minimum=301, maximum=399, chip=0x12):
        # A whole image, if one without segments: the header, padded to its checksum.
        header = bytearray(32)
        header[:2] = b"\xe9\x00"
        struct.pack_into("<H", header, 12, chip)
        struct.pack_into("<HH", header, 15, minimum, maximum)
        (self.out / name).write_bytes(header)

    def write_args(self, args=None):
        self.args_path.write_text(json.dumps(self.args if args is None else args), encoding="utf-8")

    def write_record(self, with_layout=True, *, product=None):
        name = product or "fixture"
        app = product.replace("-", "_") if product else "xiaotai"
        profile = load_profile(product) if product else {"board": "wt99p461-s1", "target": "esp32p4"}
        compiled = []
        if product:
            self.args["extra_esptool_args"]["chip"] = profile["target"]
            boot = self.args["bootloader"]
            boot["offset"] = "0x0"
            self.args["flash_files"]["0x0"] = self.args["flash_files"].pop("0x2000")
            self.args["flash_files"]["0x10000"] = self.args["app"]["file"] = f"{app}.bin"
            (self.out / "xiaotai.bin").rename(self.out / f"{app}.bin")
            self.write_args()
            compiled = [{"file": str(self.out / path)} for path in
                        ("project/main/main.c", "generated/xiaotai_selected.c")]
        inputs = {}
        for file, content in (
            ("sdkconfig", ""), ("build.ninja", ""), ("compile_commands.json", json.dumps(compiled)),
            ("CMakeCache.txt", "CMAKE_NM:FILEPATH=nm\n"), ("config/sdkconfig.h", ""),
            ("toolchain/cflags", ""),
        ):
            path = self.out / file
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            inputs[str(path)] = sha256(path)
        manifest = self.out / "build-inputs.json"
        manifest.write_text(json.dumps(inputs), encoding="utf-8")
        elf = self.out / f"{app}.elf"
        elf.write_bytes(b"fixture")
        record = {
            "name": name, "board": profile["board"], "stage": "firmware", "target": profile["target"],
            "hardware_tested": False, "sdkconfig": str(self.out / "sdkconfig"),
            "sdkconfig_sha256": inputs[str(self.out / "sdkconfig")],
            "input_manifest_sha256": sha256(manifest), "input_file_count": len(inputs),
            "artifacts": {file: sha256(self.out / file) for file in (f"{app}.bin", f"{app}.elf")},
        }
        if product:
            lock = self.out / "project/dependencies.lock"
            lock.parent.mkdir()
            lock.write_text("fixture lock\n", encoding="utf-8")
            record.update(entry="sdk", idf_line=profile["idf_line"],
                          idf_environment={"version": profile["idf_version"]},
                          tirtc_package=profile["tirtc_package"], dependencies_lock_sha256=sha256(lock))
        if with_layout:
            record["flash_layout"] = flash_layout(self.out, profile["target"], app_name=f"{app}.bin")
        (self.out / "build-record.json").write_text(json.dumps(record), encoding="utf-8")

    def test_records_all_segments(self):
        result = flash_layout(self.out, "esp32p4")
        self.assertEqual(result["app_offset"], "0x10000")
        self.assertEqual(result["flasher_args_sha256"], sha256(self.args_path))
        self.assertEqual([row["offset"] for row in result["segments"]], ["0x2000", "0x8000", "0xd000", "0x10000"])
        for row in result["segments"]:
            self.assertEqual(row["sha256"], sha256(self.out / row["file"]))

    def test_product_named_app_only_accepted_with_explicit_identity(self):
        product_app = "xiaotai_szpi_s3.bin"
        (self.out / "xiaotai.bin").rename(self.out / product_app)
        self.args["flash_files"]["0x10000"] = product_app
        self.args["app"]["file"] = product_app
        self.write_args()
        with self.assertRaisesRegex(ValueError, "Flash app must be xiaotai.bin"):
            flash_layout(self.out, "esp32p4")
        result = flash_layout(self.out, "esp32p4", app_name=product_app)
        self.assertEqual(result["segments"][-1]["file"], product_app)
        with self.assertRaisesRegex(ValueError, "Flash app must be another.bin"):
            flash_layout(self.out, "esp32p4", app_name="another.bin")

    def test_rejects_bad_chip_missing_files_and_segment_overlap(self):
        with self.assertRaisesRegex(ValueError, "Flash target differs"):
            flash_layout(self.out, "esp32c3")
        for offset, value, expected in (
            ("0x7000", "../other.bin", "outside build directory"),
            ("0x2001", "ota_data_initial.bin", "not sector aligned"),
            ("0x02000", "ota_data_initial.bin", "Overlapping flash segment"),
            ("0x1000000", "ota_data_initial.bin", "exceeds 16MB"),
            ("0x7000", "missing.bin", "Missing or empty"),
        ):
            with self.subTest(offset=offset, value=value):
                args = copy.deepcopy(self.args)
                args["flash_files"][offset] = value
                self.write_args(args)
                with self.assertRaisesRegex(ValueError, expected):
                    flash_layout(self.out, "esp32p4")
        self.write_args(dict(self.args, app={"offset": "0x11000", "file": "xiaotai.bin"}))
        with self.assertRaisesRegex(ValueError, "app does not match"):
            flash_layout(self.out, "esp32p4")

    def test_rejects_ambiguous_json(self):
        self.args_path.write_text('{"flash_files": {}, "flash_files": {}}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Duplicate key"):
            flash_layout(self.out, "esp32p4")

    def test_verifier_rejects_same_images_with_changed_offset_or_partition(self):
        self.write_record()
        with patch("verify_build.subprocess.check_output", return_value="0 T xiaotai::board::SelectedBoard()\n"):
            self.assertTrue(verify(self.out, require_flash_layout=True)["flash_layout_verified"])
        original_app = sha256(self.out / "xiaotai.bin")
        args = copy.deepcopy(self.args)
        args["app"]["offset"] = "0x11000"
        args["flash_files"]["0x11000"] = args["flash_files"].pop("0x10000")
        self.write_args(args)
        self.assertEqual(sha256(self.out / "xiaotai.bin"), original_app)
        with self.assertRaisesRegex(ValueError, "Flash layout changed"):
            verify(self.out, require_flash_layout=True)
        self.write_args()
        (self.out / "partition_table/partition-table.bin").write_bytes(b"foreign partition data")
        with self.assertRaisesRegex(ValueError, "Flash layout changed"):
            verify(self.out, require_flash_layout=True)

    def test_legacy_record_can_be_read_but_not_used_for_flash_preflight(self):
        self.write_record(with_layout=False)
        with patch("verify_build.subprocess.check_output", return_value="0 T xiaotai::board::SelectedBoard()\n"):
            self.assertFalse(verify(self.out)["flash_layout_verified"])
        with self.assertRaisesRegex(ValueError, "No recorded firmware flash layout"):
            verify(self.out, require_flash_layout=True)

    def test_native_record_uses_named_app_and_audits_all_segments(self):
        self.write_record(product="xiaotai-szpi-s3")
        elf = self.out / "xiaotai_szpi_s3.elf"
        symbols = "0 T xiaotai_start\n1 T xiaotai_at_service\n2 T TiRtcInit\n"
        demangled = "0 T xiaotai::board::SelectedBoard()\n1 T app_main\n"
        image = f"ELF file SHA256: {sha256(elf)}\nValidation hash: fixture (valid)\n"
        board = "lckfb/szpi-esp32s3"
        with patch("verify_build.subprocess.check_output", side_effect=[symbols, demangled, image]):
            result = verify(self.out, require_flash_layout=True, expected_board=board, expected_entry="sdk")
        self.assertTrue(result["flash_layout_verified"])
        self.assertTrue(result["elf_image_identity_verified"])
        with self.assertRaisesRegex(ValueError, "Build entry differs"):
            verify(self.out, require_flash_layout=True, expected_board=board, expected_entry="legacy")
        (self.out / "partition_table/partition-table.bin").write_bytes(b"foreign partition data")
        with self.assertRaisesRegex(ValueError, "Flash layout changed"):
            verify(self.out, require_flash_layout=True, expected_board=board, expected_entry="sdk")

    def test_native_lvgl_ends_the_program_where_it_cannot_go_on(self):
        self.write_record(product="xiaotai-szpi-s3")
        elf = self.out / "xiaotai_szpi_s3.elf"
        symbols = "0 T xiaotai_start\n1 T xiaotai_at_service\n2 T TiRtcInit\n"
        demangled = "0 T xiaotai::board::SelectedBoard()\n1 T app_main\n"
        image = f"ELF file SHA256: {sha256(elf)}\nValidation hash: fixture (valid)\n"
        commands = self.out / "compile_commands.json"
        compiled = json.loads(commands.read_text(encoding="utf-8"))
        source = str(self.out / "project/managed_components/lvgl__lvgl/src/draw/lv_draw.c")

        def check(command):
            commands.write_text(json.dumps(compiled + [{"file": source, "command": command}]), encoding="utf-8")
            # The record names what was built from: the changed list is that build's.
            manifest, kept = self.out / "build-inputs.json", self.out / "build-record.json"
            inputs = json.loads(manifest.read_text(encoding="utf-8"))
            inputs[str(commands)] = sha256(commands)
            manifest.write_text(json.dumps(inputs), encoding="utf-8")
            record = json.loads(kept.read_text(encoding="utf-8"))
            record["input_manifest_sha256"] = sha256(manifest)
            kept.write_text(json.dumps(record), encoding="utf-8")
            with patch("verify_build.subprocess.check_output", side_effect=[symbols, demangled, image]):
                return verify(self.out, require_flash_layout=True, expected_entry="sdk")

        # As the build system writes it for the shell, quoted or escaped, and as plain arguments.
        for command in ('cc -O2 "-DLV_ASSERT_HANDLER=while(abort(),1){}" -c lv_draw.c',
                        "cc -DLV_ASSERT_HANDLER=while(abort(),1){} -c lv_draw.c",
                        "cc -DLV_ASSERT_HANDLER=while" + chr(92) + "(abort" + chr(92) + "(" + chr(92) + ")," + "1"
                        + chr(92) + ")" + chr(92) + "{" + chr(92) + "} -c lv_draw.c"):
            self.assertEqual(check(command)["lvgl_sources_ending_the_program"], 1)
        for command in ("cc -O2 -c lv_draw.c", "cc -DLV_ASSERT_HANDLER=while(1); -c lv_draw.c"):
            with self.assertRaisesRegex(ValueError, "LVGL would halt where it cannot go on: lv_draw.c"):
                check(command)

    def test_same_chip_does_not_replace_board_identity(self):
        self.write_record()
        with self.assertRaisesRegex(ValueError, "Build board differs"):
            verify(self.out, require_flash_layout=True,
                   expected_board="waveshare/esp32-p4-wifi6-touch-lcd-3.5")

    def test_p4_preflight_reports_the_common_rev3_range(self):
        self.write_p4_image("xiaotai.bin", minimum=302)
        self.write_p4_image("bootloader/bootloader.bin", maximum=305)
        self.write_record()
        with patch("verify_build.subprocess.check_output", return_value="0 T xiaotai::board::SelectedBoard()\n"):
            result = verify(self.out, require_flash_layout=True)
        self.assertEqual(result["p4_chip_revision_range"], {"min": 302, "max": 305})

    def test_p4_preflight_rejects_revision_and_chip_even_with_matching_hashes(self):
        for name, minimum, maximum, chip, error in (
            ("xiaotai.bin", 100, 199, 0x12, "outside v3.1-v3.99"),
            ("bootloader/bootloader.bin", 100, 399, 0x12, "outside v3.1-v3.99"),
            ("xiaotai.bin", 300, 399, 0x12, "outside v3.1-v3.99"),
            ("bootloader/bootloader.bin", 301, 400, 0x12, "outside v3.1-v3.99"),
            ("xiaotai.bin", 399, 301, 0x12, "outside v3.1-v3.99"),
            ("xiaotai.bin", 301, 399, 0x09, "not an ESP32-P4 image"),
        ):
            with self.subTest(image=name, minimum=minimum, maximum=maximum, chip=chip):
                self.write_p4_image(name, minimum, maximum, chip)
                self.write_record()
                with patch("verify_build.subprocess.check_output", return_value="0 T xiaotai::board::SelectedBoard()\n"), \
                        self.assertRaisesRegex(ValueError, error):
                    verify(self.out, require_flash_layout=True)
                self.write_p4_image(name)

    def test_p4_preflight_rejects_a_short_header_or_disjoint_revisions(self):
        (self.out / "xiaotai.bin").write_bytes(b"\xe9")
        self.write_record()
        with patch("verify_build.subprocess.check_output", return_value="0 T xiaotai::board::SelectedBoard()\n"), \
                self.assertRaisesRegex(ValueError, "Short ESP image header"):
            verify(self.out, require_flash_layout=True)
        self.write_p4_image("xiaotai.bin", minimum=305)
        self.write_p4_image("bootloader/bootloader.bin", maximum=304)
        self.write_record()
        with patch("verify_build.subprocess.check_output", return_value="0 T xiaotai::board::SelectedBoard()\n"), \
                self.assertRaisesRegex(ValueError, "no common chip revision"):
            verify(self.out, require_flash_layout=True)

    def test_p4_preflight_checks_the_bootloader_selected_by_idf(self):
        selected = "bootloader/alternate.bin"
        self.write_p4_image(selected, minimum=100)
        self.args["flash_files"]["0x2000"] = selected
        self.args["bootloader"]["file"] = selected
        self.write_args()
        self.write_record()
        with self.assertRaisesRegex(ValueError, r"alternate\.bin.*outside v3.1-v3.99"):
            verify(self.out, require_flash_layout=True)


if __name__ == "__main__":
    unittest.main()
