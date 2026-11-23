from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import xt
import partition_capture as live
from flash_layout import flash_layout
from partition_plan import ENTRY, ERASED, MD5_TAG, TABLE_BYTES, parse_table, plan


LIMIT = 16 * 1024 * 1024
NVS = (1, 2, 0x9000, 0x6000, b"nvs", 0)
PHY = (1, 1, 0xF000, 0x1000, b"phy_init", 0)
APP = (0, 0, 0x10000, 0xFF0000, b"factory", 0)


def table(*rows):
    body = b"".join(ENTRY.pack(0x50AA, *row) for row in rows)
    end = MD5_TAG + hashlib.md5(body, usedforsecurity=False).digest()
    return (body + end).ljust(TABLE_BYTES, b"\xff")


def fake_ports(port="COM25", serial="synthetic-iface"):
    return [SimpleNamespace(device=port, vid=0x303A, pid=0x1001, serial_number=serial)]


class FakeRom:
    IS_STUB = sync_stub_detected = secure_download_mode = False

    def __init__(self, wire, baud, data):
        if baud != 115200 or not wire.is_open:
            raise AssertionError("explicit opened local serial required")
        self.data = data
        self.calls = []
        self.info = {"chip_id": 9, "flash_crypt_cnt": 0,
                     "parsed_flags": {"SECURE_DOWNLOAD_ENABLE": False, "SECURE_BOOT_EN": False}}
        self.mac = (2, 0, 0, 0, 0, 1)
        self.rev = (0, 2)

    def connect(self, **opts):
        self.calls.append(("rom", opts))

    def get_security_info(self):
        return self.info

    def read_mac(self):
        return self.mac

    def get_major_chip_version(self):
        return self.rev[0]

    def get_minor_chip_version(self):
        return self.rev[1]

    def flash_spi_attach(self, pins):
        self.calls.append(("attach", pins))

    def flash_set_parameters(self, size):
        self.calls.append(("params", size))

    def read_flash(self, start, size):
        self.calls.append(("read", start, size))
        return self.data

    def hard_reset(self):
        self.calls.append(("reset",))


class FakeSerial:
    def __init__(self, **opts):
        assert opts == {"port": None, "baudrate": 115200, "exclusive": True, "write_timeout": 2}
        self.is_open = False
        self.opened = []

    def open(self):
        assert self.dtr is False and self.rts is False
        self.is_open = True
        self.opened.append(self.port)

    def close(self):
        self.is_open = False


class PartitionPlanTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="xt partition plan ")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.saved = self.root / "board.bin"
        self.saved.write_bytes(table(NVS, PHY, APP))
        self.manifest = self.build / "flasher_args.json"
        self.record = self.build / "build-record.json"
        self.write_build(self.saved.read_bytes())

    def image(self, name, chip_id, is_app=False, size=64):
        block = bytearray(size)
        block[0] = 0xE9
        struct.pack_into("<H", block, 12, chip_id)
        if is_app:
            struct.pack_into("<I", block, 32, 0xABCD5432)
        (self.build / name).write_bytes(block)

    def write_build(self, partition, *, target="esp32s3", app_offset="0x10000", app_size=64):
        chip_id = {"esp32s3": 9, "esp32p4": 0x12}[target]
        self.image("boot.bin", chip_id)
        self.image("app.bin", chip_id, is_app=True, size=app_size)
        (self.build / "partition.bin").write_bytes(partition)
        offsets = {"bootloader": ("0x2000" if target == "esp32p4" else "0x0", "boot.bin"),
                   "partition-table": ("0x8000", "partition.bin"), "app": (app_offset, "app.bin")}
        args = {"extra_esptool_args": {"chip": target},
                "flash_settings": {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"},
                "flash_files": dict(offsets.values())}
        args.update({key: {"offset": value[0], "file": value[1], "encrypted": "false"}
                     for key, value in offsets.items()})
        self.manifest.write_text(json.dumps(args), encoding="utf-8")
        self.record_build(target)

    def record_build(self, target):
        layout = flash_layout(self.build, target, app_name="app.bin")
        digest = next(row["sha256"] for row in layout["segments"] if row["file"] == "app.bin")
        self.record.write_text(json.dumps({"stage": "firmware", "target": target, "board": "test/board",
                                           "flash_layout": layout, "artifacts": {"app.bin": digest}}), encoding="utf-8")

    def test_app_only_argv_is_a_preview(self):
        report = plan(self.build, self.saved)
        self.assertEqual(report["mode"], "app-only")
        self.assertFalse(report["write_authorized"])
        self.assertIsNone(report["esptool_preview_argv"])
        self.assertEqual(report["built_table_sha256"], report["saved_table_sha256"])
        self.assertTrue(report["nvs_layout_unchanged"] and report["phy_layout_unchanged"])
        report = plan(self.build, self.saved, port="COM25")
        self.assertEqual(report["esptool_preview_argv"][-3:],
                         ["write-flash", "0x10000", str(self.build / "app.bin")])

    def test_three_segments_for_factory_change_only(self):
        changed_app = (0, 0, 0x20000, 0xFE0000, b"factory", 0)
        self.write_build(table(NVS, PHY, changed_app), target="esp32p4", app_offset="0x20000")
        report = plan(self.build, self.saved, port="COM15")
        self.assertEqual(report["mode"], "three-segment")
        self.assertEqual([row["label"] for row in report["changes"]], ["factory"])
        self.assertEqual(report["esptool_preview_argv"][-6::2], ["0x2000", "0x8000", "0x20000"])
        self.assertEqual(report["preview_segments"][1]["erase_end_exclusive"], "0x9000")
        for forbidden in ("erase-flash", "--erase-all", "--force"):
            self.assertNotIn(forbidden, report["esptool_preview_argv"])

    def test_data_change_never_generates_an_erase_or_write(self):
        new_nvs = (1, 2, 0x9000, 0x5000, b"nvs", 0)
        new_phy = (1, 1, 0xE000, 0x1000, b"phy_init", 0)
        keys = (1, 4, 0xE000, 0x1000, b"nvs_keys", 0)
        for data in ((new_nvs, PHY), (new_nvs, new_phy), (NVS,), (new_nvs, keys, PHY), (PHY,)):
            with self.subTest(data=data):
                self.write_build(table(*data, APP))
                report = plan(self.build, self.saved, port="COM25")
                self.assertEqual(report["mode"], "manual-full-review")
                self.assertFalse(report["data_layout_unchanged"])
                self.assertIsNone(report["esptool_preview_argv"])

    def test_no_phy_is_valid_when_both_lack_it(self):
        self.saved.write_bytes(table(NVS, APP))
        self.write_build(self.saved.read_bytes())
        self.assertEqual(plan(self.build, self.saved)["mode"], "app-only")

    def test_bad_dump_is_never_a_plan(self):
        good = table(NVS, PHY, APP)
        body, md5 = good[:96], good[96:128]
        cut = 128
        for bad in (b"", good[:-1], good + b"\xff" * 1024, ERASED * 96,
                    good[:127] + bytes([good[127] ^ 1]) + good[128:],
                    body.ljust(TABLE_BYTES, b"\xff"),
                    body + md5 + md5 + good[cut + 32:],
                    good[:cut] + good[:32] + good[cut + 32:],
                    good[:-1] + b"\x00", good[:cut] + b"\x00" * (TABLE_BYTES - cut)):
            with self.subTest(length=len(bad), start=bad[:4]):
                self.saved.write_bytes(bad)
                with self.assertRaises(ValueError):
                    plan(self.build, self.saved, port="COM25")

    def test_ranges_and_labels(self):
        for bad in ((1, 3, 0xE000, 0x1000, b"overlap", 0), (1, 3, 0x8000, 0x1000, b"header", 0),
                    (1, 3, 0x9001, 0x1000, b"misaligned", 0), (1, 3, 0xD000, 0, b"empty", 0),
                    (1, 3, 0xFFFFF000, 0x2000, b"overflow", 0), (0, 0, 0x1F000, 0x1000, b"align", 0),
                    (1, 3, 0xF000, 0x1000, b"nvs", 0), (1, 3, 0xF000, 0x1000, b"nvs\0fake", 0)):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                parse_table(table(NVS, bad), LIMIT, "fixture")

    def test_unsupported_boot_or_security_policy(self):
        ota_data = (1, 0, 0xF000, 0x1000, b"otadata", 0)
        ota_app = (0, 0x10, 0x10000, 0xFF0000, b"ota_0", 0)
        encrypted_nvs = (1, 2, 0x9000, 0x6000, b"nvs", 1)
        for rows in ((NVS, ota_data, APP), (NVS, PHY, ota_app), (encrypted_nvs, PHY, APP)):
            with self.subTest(rows=rows), self.assertRaisesRegex(ValueError, "single-factory"):
                self.saved.write_bytes(table(*rows))
                plan(self.build, self.saved, port="COM25")

    def test_bad_app_or_artifact(self):
        with self.assertRaisesRegex(ValueError, "does not fit"):
            self.write_build(table(NVS, PHY, (0, 0, 0x10000, 0x1000, b"factory", 0)), app_size=0x1001)
            plan(self.build, self.saved)
        self.write_build(table(NVS, PHY, APP), app_offset="0x20000")
        with self.assertRaisesRegex(ValueError, "does not fit"):
            plan(self.build, self.saved)
        self.write_build(table(NVS, PHY, APP))
        self.image("app.bin", 0x12, is_app=True)
        with self.assertRaisesRegex(ValueError, "verified build record"):
            plan(self.build, self.saved)
        self.record_build("esp32s3")
        with self.assertRaisesRegex(ValueError, "not a esp32s3 app image"):
            plan(self.build, self.saved)

    def test_manifest_and_port(self):
        args = json.loads(self.manifest.read_text())
        args["app"]["encrypted"] = "true"
        self.manifest.write_text(json.dumps(args), encoding="utf-8")
        self.record_build("esp32s3")
        with self.assertRaisesRegex(ValueError, "encrypted"):
            plan(self.build, self.saved)
        self.write_build(table(NVS, PHY, APP))
        with self.assertRaisesRegex(ValueError, "explicit COM or /dev"):
            plan(self.build, self.saved, port="--force")
        self.manifest.write_text('{"flash_files":{}, "flash_files":{}}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            plan(self.build, self.saved)

    def test_extra_segments_and_escaped_inputs(self):
        args = json.loads(self.manifest.read_text())
        (self.build / "extra.bin").write_bytes(b"extra data")
        args["flash_files"]["0x9000"] = "extra.bin"
        self.manifest.write_text(json.dumps(args), encoding="utf-8")
        self.record_build("esp32s3")
        with self.assertRaisesRegex(ValueError, "only a dedicated"):
            plan(self.build, self.saved)
        args["flash_files"]["0x9000"] = "../board.bin"
        self.manifest.write_text(json.dumps(args), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "outside build"):
            plan(self.build, self.saved)

    def test_cli_no_serial_or_process(self):
        sysargs = ["xt.py", "partition-plan", "--build-dir", str(self.build),
                   "--board-table", str(self.saved), "--port", "COM25"]
        with (patch("sys.argv", sysargs), patch.object(xt, "serial", None),
              patch.object(xt, "Port", side_effect=AssertionError("no serial")),
              patch.object(xt, "resolve", side_effect=AssertionError("no device lookup")),
              patch.object(xt.subprocess, "run", side_effect=AssertionError("no subprocess")),
              patch("socket.socket", side_effect=AssertionError("no network"))):
            with redirect_stdout(io.StringIO()) as out:
                xt.main()
            self.assertFalse(json.loads(out.getvalue())["write_authorized"])
            self.saved.write_bytes(table((1, 2, 0x9000, 0x5000, b"nvs", 0), PHY, APP))
            with redirect_stdout(io.StringIO()) as out, self.assertRaises(SystemExit) as blocked:
                xt.main()
            self.assertEqual(blocked.exception.code, 1)
            self.assertIsNone(json.loads(out.getvalue())["esptool_preview_argv"])
            self.saved.write_bytes(b"incomplete")
            with redirect_stdout(io.StringIO()) as out, redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                xt.main()
            self.assertEqual(out.getvalue(), "")

    def live_args(self):
        return ["xt.py", "partition-plan", "--build-dir", str(self.build), "--read-table", "--port", "COM25",
                "--expect-board", "test/board", "--expect-chip", "esp32s3", "--usb-vid-pid", "303a:1001",
                "--usb-serial", "synthetic-iface", "--rom-mac", "02:00:00:00:00:01", "--revision", "0.2",
                "--allow", live.ACTION]

    def live_target(self):
        return live.request([live.ACTION], "COM25", "test/board", "esp32s3", "303a:1001", "synthetic-iface",
                            "02:00:00:00:00:01", "0.2")

    def invoke_live(self, *, suffix=(), adjust=None, size="16MB", ports=fake_ports):
        opened = []
        connection = []

        def make_serial(**opts):
            opened.append(FakeSerial(**opts))
            return opened[-1]

        def make_rom(wire, baud):
            rom = FakeRom(wire, baud, self.saved.read_bytes())
            connection.append(rom)
            if adjust:
                adjust(rom)
            return rom

        with (patch("sys.argv", self.live_args() + list(suffix)),
              patch.object(xt, "serial", SimpleNamespace(Serial=make_serial)),
              patch.object(xt, "list_ports", SimpleNamespace(comports=ports)),
              patch.object(xt, "Port", side_effect=AssertionError("no AT console")),
              patch.object(xt, "resolve", side_effect=AssertionError("no device registry")),
              patch.object(xt.subprocess, "run", side_effect=AssertionError("no child/write command")),
              patch("socket.socket", side_effect=AssertionError("no network")),
              patch.object(live, "esptool_adapter", return_value=(make_rom, lambda _rom: size)),
              redirect_stderr(io.StringIO()) as err, redirect_stdout(io.StringIO()) as out):
            try:
                xt.main()
                code = 0
            except SystemExit as error:
                code = error.code
        return code, out.getvalue(), err.getvalue(), opened, connection

    def test_live_only_fixed_range_then_reset_and_close(self):
        code, out, err, opened, connection = self.invoke_live(ports=lambda: fake_ports() + fake_ports("COM42", "other"))
        self.assertEqual(code, 0, err)
        report = json.loads(out)
        self.assertEqual(report["mode"], "app-only")
        self.assertFalse(report["write_authorized"])
        self.assertTrue(report["capture"]["hard_reset_requested"])
        self.assertFalse(report["capture"]["app_boot_verified"])
        self.assertEqual(report["saved_table_sha256"], hashlib.sha256(self.saved.read_bytes()).hexdigest())
        self.assertEqual(opened[0].opened, ["COM25"])
        self.assertFalse(opened[0].is_open)
        self.assertEqual(connection[0].calls, [("rom", {"mode": "default-reset", "attempts": 1, "warnings": False}),
                                               ("attach", 0), ("params", LIMIT), ("read", 0x8000, 3072), ("reset",)])

    def test_live_arguments_and_wrong_usb_fail_before_ever_opening(self):
        for suffix, ports in ((["--allow", live.ACTION], fake_ports), (["--port", "socket://x:1"], fake_ports),
                              ([], lambda: fake_ports("COM42")), ([], lambda: fake_ports() + fake_ports("COM42")),
                              (["--expect-board", "other/board"], fake_ports), (["--expect-chip", "esp32p4"], fake_ports),
                              (["--board-table", str(self.saved)], fake_ports), (["--offset", "0x0"], fake_ports)):
            with self.subTest(suffix=suffix, ports=ports):
                code, out, _, opened, connection = self.invoke_live(suffix=suffix, ports=ports)
                self.assertNotEqual(code, 0)
                self.assertEqual((out, opened, connection), ("", [], []))
        with self.assertRaisesRegex(ValueError, "rev3"):
            live.request([live.ACTION], "COM15", "wt/wt99p461-s1", "esp32p4", "1a86:55d3", "synthetic-iface",
                         "02:00:00:00:00:01", "1.3")

    def test_live_wrong_rom_identity_or_stub_no_flash_operations(self):
        def wrong_chip(rom):
            rom.info["chip_id"] = 18

        for adjust in (wrong_chip, lambda rom: setattr(rom, "mac", (2, 0, 0, 0, 0, 2)),
                       lambda rom: setattr(rom, "sync_stub_detected", True),
                       lambda rom: rom.info["parsed_flags"].update(SECURE_DOWNLOAD_ENABLE=True)):
            with self.subTest(adjust=adjust):
                code, out, err, opened, connection = self.invoke_live(adjust=adjust)
                self.assertNotEqual(code, 0)
                self.assertEqual(out, "")
                self.assertIn("verify device boot manually", err)
                self.assertEqual([c[0] for c in connection[0].calls], ["rom"])
                self.assertFalse(opened[0].is_open)

    def test_live_bad_revision_security_capacity_reject_read(self):
        for adjust, size, expected in (
                (lambda rom: setattr(rom, "rev", (0, 1)), "16MB", "revision"),
                (lambda rom: rom.info["parsed_flags"].update(SECURE_BOOT_EN=True), "16MB", "security policy"),
                (lambda rom: rom.info.update(flash_crypt_cnt=7), "16MB", "security policy"),
                (lambda rom: rom.info.pop("flash_crypt_cnt"), "16MB", "security policy"),
                (None, "8MB", "capacity"), (None, None, "capacity")):
            with self.subTest(adjust=adjust, size=size):
                code, out, err, opened, connection = self.invoke_live(adjust=adjust, size=size)
                self.assertNotEqual(code, 0)
                self.assertEqual(out, "")
                self.assertIn(expected, err)
                self.assertNotIn("read", [c[0] for c in connection[0].calls])
                self.assertEqual(connection[0].calls[-1], ("reset",))
                self.assertFalse(opened[0].is_open)

    def test_live_truncated_or_malformed_dump_no_preview_but_reset(self):
        for raw, expected in ((bytes(4), "length"), (bytes(TABLE_BYTES), "record")):
            with self.subTest(length=len(raw)):
                self.saved.write_bytes(raw)
                code, out, err, opened, connection = self.invoke_live()
                self.assertNotEqual(code, 0)
                self.assertEqual(out, "")
                self.assertIn(expected, err)
                self.assertEqual(connection[0].calls[-2:], [("read", 0x8000, 3072), ("reset",)])
                self.assertFalse(opened[0].is_open)

    def test_live_cleanup_failure_never_claims_plan(self):
        def break_reset(rom):
            def failed():
                raise OSError("fixture reset failed")

            rom.hard_reset = failed

        code, out, err, opened, connection = self.invoke_live(adjust=break_reset)
        self.assertNotEqual(code, 0)
        self.assertEqual(out, "")
        self.assertIn("hard reset failed; check the board manually", err)
        self.assertEqual(connection[0].calls[-1], ("read", 0x8000, 3072))
        self.assertFalse(opened[0].is_open)

    def test_live_changed_build_never_claims_stale_preview(self):
        def change_on_read(rom):
            read = rom.read_flash

            def read_and_change(start, size):
                data = read(start, size)
                self.image("app.bin", 9, is_app=True, size=128)
                self.record_build("esp32s3")
                return data

            rom.read_flash = read_and_change

        code, out, err, opened, connection = self.invoke_live(adjust=change_on_read)
        self.assertNotEqual(code, 0)
        self.assertEqual(out, "")
        self.assertIn("verified build changed", err)
        self.assertEqual(connection[0].calls[-2:], [("read", 0x8000, 3072), ("reset",)])
        self.assertFalse(opened[0].is_open)


if __name__ == "__main__":
    unittest.main()
