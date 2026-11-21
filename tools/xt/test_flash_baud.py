"""Exercise xt flash with mocked enumeration and subprocesses only."""

import contextlib
from collections import deque
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

with patch.dict(sys.modules, {"serial": types.ModuleType("serial"),
                              "serial.tools": types.SimpleNamespace(list_ports=None)}):
    import xt
from flash_baud import RATE_ERROR, file_identity


def usb(port="COM27", vid=0x1A86, pid=0x55D3, serial="fixture-CH343", location="1-4.2"):
    return types.SimpleNamespace(device=port, vid=vid, pid=pid, serial_number=serial, location=location)


def rate_failure(stream="stderr", extra="", code=2):
    out = "Changing baud rate to 921600...\nChanged.\n" + extra
    if stream == "stdout":
        return code, out + RATE_ERROR + "\n", ""
    return code, out, RATE_ERROR + "\n"


class FlashBaudTest(unittest.TestCase):
    def test_windows_path_and_handle_share_birth_time_not_ctime(self):
        same_file = dict(st_dev=1, st_ino=2, st_size=3, st_mtime_ns=4, st_birthtime_ns=5)
        path_stat = types.SimpleNamespace(**same_file, st_ctime_ns=5)
        handle_stat = types.SimpleNamespace(**same_file, st_ctime_ns=4)
        self.assertEqual(file_identity(path_stat, windows=True), file_identity(handle_stat, windows=True))
        self.assertNotEqual(file_identity(path_stat, windows=False), file_identity(handle_stat, windows=False))
        handle_stat.st_ino += 1
        self.assertNotEqual(file_identity(path_stat, windows=True), file_identity(handle_stat, windows=True))

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.image = Path(temporary.name) / "board app.bin"
        self.image.write_bytes(b"fixture image")

    def invoke(self, ports, results, *, baud=None, serial=None, mutate=None, target="COM27"):
        snapshots = deque(ports)
        programs = []
        replies = deque(results)
        info = {"port": target, "kind": "sdk", "usb": serial}
        args = types.SimpleNamespace(dev="fixture", chip="esp32p4", bin=str(self.image), baud=baud, offset="0x10000")
        out, err = io.StringIO(), io.StringIO()

        def mock_run(argv, **kwargs):
            if argv[1:3] == ["-c", "import esptool"]:
                return subprocess.CompletedProcess(argv, 0)
            self.assertEqual(argv[1:7], ["-m", "esptool", "--chip", "esp32p4", "--port", target])
            self.assertEqual(argv[-3:], ["write_flash", "0x10000", str(self.image)])
            self.assertEqual(kwargs["encoding"], "utf-8")
            programs.append(argv)
            value = replies.popleft()
            if mutate is not None and len(programs) == 1:
                mutate()
            return subprocess.CompletedProcess(argv, *value)

        with (patch.object(xt, "resolve", return_value=("fixture", info)),
              patch.object(xt, "list_ports", types.SimpleNamespace(comports=lambda: snapshots.popleft())),
              patch.object(xt.subprocess, "run", side_effect=mock_run),
              contextlib.redirect_stdout(out), contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as exit):
            xt.cmd_flash(args)
        return exit.exception.code, programs, err.getvalue(), len(snapshots)

    def test_confirmed_bridge_default_and_explicit_baud(self):
        for explicit, expected, snapshots in ((None, "921600", [[usb()]]), (230400, "230400", [])):
            with self.subTest(explicit=explicit):
                code, runs, _, left = self.invoke(snapshots, [(0, "", "")], baud=explicit)
                self.assertEqual((code, [r[8] for r in runs], left), (0, [expected], 0))

    def test_native_unknown_and_ambiguous_ports_keep_legacy_default(self):
        cases = [[], [usb(vid=0x303A, pid=0x1001)], [usb(pid=0x55D4)], [usb(), usb()]]
        for ports in cases:
            with self.subTest(ports=ports):
                code, runs, _, _ = self.invoke([ports], [rate_failure()])
                self.assertEqual((code, [r[8] for r in runs]), (2, ["460800"]))

    def test_exact_v4_or_v5_baud_error_replays_identical_argv_except_rate(self):
        for stream in ("stdout", "stderr"):
            with self.subTest(stream=stream):
                code, (first, second), error, left = self.invoke([[usb()], [usb(port="com27")]],
                    [rate_failure(stream), (0, "", "")])
                self.assertEqual((code, first[8], second[8], left), (0, "921600", "115200", 0))
                self.assertEqual(first[:8] + first[9:], second[:8] + second[9:])
                self.assertIn("same flash once", error)

    def test_explicit_921600_disables_automatic_fallback(self):
        code, runs, error, left = self.invoke([], [rate_failure()], baud=921600)
        self.assertEqual((code, [r[8] for r in runs], left), (2, ["921600"], 0))
        self.assertNotIn("Retrying", error)

    def test_transport_unproven_or_protected_failure_is_not_replayed(self):
        for extra in ("Writing at 0x10000\n", "Flash will be erased\n", "Verify failed\n",
                      "Secure boot is enabled\n", "Wrong chip\n", "Invalid argument\n", "Aborted\n",
                      "A fatal error occurred: Failed to connect to ESP32\n"):
            with self.subTest(extra=extra):
                code, runs, _, _ = self.invoke([[usb()]], [rate_failure(extra=extra)])
                self.assertEqual((code, len(runs)), (2, 1))
        for result in (rate_failure(code=1), (2, "", RATE_ERROR),
                       (2, "Changing baud rate to 921600...\n", "A fatal error occurred: Invalid head of packet (0x00)")):
            with self.subTest(result=result):
                code, runs, _, _ = self.invoke([[usb()]], [result])
                self.assertEqual((code, len(runs)), (result[0], 1))

    def test_changed_or_ambiguous_usb_blocks_replay(self):
        by_location = [usb(location="2-4.2")]
        by_serial = [usb(serial="different-board")]
        by_port = [usb(port="COM12")]
        by_pid = [usb(pid=0x55D4)]
        for after in ([], by_location, by_serial, by_port, by_pid):
            with self.subTest(after=after):
                code, runs, error, _ = self.invoke([[usb()], after], [rate_failure()])
                self.assertEqual((code, len(runs)), (2, 1))
                self.assertIn("No retry", error)
        for initial in ([usb(serial=None)], [usb(location=None)], [usb(serial="00000000")],
                        [usb(), usb(port="COM12")]):
            with self.subTest(initial=initial):
                code, runs, error, _ = self.invoke([initial, [usb()]], [rate_failure()])
                self.assertEqual((code, [r[8] for r in runs]), (2, ["921600"]))
                self.assertIn("No retry", error)

    def test_image_content_replacement_or_disappearance_blocks_replay(self):
        before = self.image.read_bytes()

        def replace():
            replacement = self.image.with_suffix(".replacement")
            replacement.write_bytes(before)
            replacement.replace(self.image)

        for mutation in (lambda: self.image.write_bytes(b"updated image"), replace, self.image.unlink):
            with self.subTest(mutation=mutation):
                self.image.write_bytes(before)
                code, runs, error, _ = self.invoke([[usb()], [usb()]], [rate_failure()], mutate=mutation)
                self.assertEqual((code, len(runs)), (2, 1))
                self.assertIn("No retry", error)

    def test_linux_port_keeps_exact_path_without_remapping(self):
        port = "/dev/ttyUSB0"
        code, runs, _, _ = self.invoke([[usb(port=port)], [usb(port=port)]],
            [rate_failure(), (0, "", "")], target=port)
        self.assertEqual((code, [r[6] for r in runs]), (0, [port, port]))

    def test_known_alias_with_changed_usb_aborts_before_flash(self):
        code, runs, _, _ = self.invoke([[usb()]], [], serial="old-fixture")
        self.assertIn("recorded USB serial", code)
        self.assertFalse(runs)

    def test_second_failure_is_terminal(self):
        code, runs, _, left = self.invoke([[usb()], [usb()]], [rate_failure(), rate_failure()])
        self.assertEqual((code, [r[8] for r in runs], left), (2, ["921600", "115200"], 0))


if __name__ == "__main__":
    unittest.main()
