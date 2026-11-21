"""Execute the real CLI gates with a serial module that forbids port access."""
from pathlib import Path
import contextlib
import io
import json
import runpy
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


class RecognitionConsoleTests(unittest.TestCase):
    def invoke(self, commands, flags=()):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "must-not-exist" / "capture.log"
            argv = ["device_console.py", "--port", "fixture", "--output", str(output), "--dry-run"]
            for command in commands:
                argv += ["--command", command]
            argv += list(flags)
            serial = types.ModuleType("serial")
            serial.Serial = lambda **_: self.fail("CLI must not open or initialize a serial port")
            stream = io.StringIO()
            with patch.dict(sys.modules, {"serial": serial}), patch.object(sys, "argv", argv), \
                 contextlib.redirect_stdout(stream), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as stopped:
                    runpy.run_path(str(ROOT / "scripts/device_console.py"), run_name="__main__")
            self.assertFalse(output.parent.exists())
            return stopped.exception.code, stream.getvalue()

    def test_read_only_and_dry_run(self):
        code, output = self.invoke(["AT+RECOGNITION CONFIG", "AT+RECOGNITION STATE"])
        self.assertEqual(code, 0)
        self.assertTrue(json.loads(output)["dry_run"])

    def test_actions_need_separate_permission(self):
        for command in ("QUERY", "CANCEL", "RESUME task", "DISMISS task item", "ARCHIVE task 0", "ARCHIVE task 9007199254740991"):
            text = "AT+RECOGNITION " + command
            self.assertEqual(self.invoke([text])[0], 2)
            self.assertEqual(self.invoke([text], ["--allow-recognition-upload"])[0], 2)
            self.assertEqual(self.invoke([text], ["--allow-recognition-actions"])[0], 0)

    def test_record_needs_upload_permission(self):
        for seconds in (1, 10, 60):
            command = f"AT+RECOGNITION RECORD {seconds}"
            self.assertEqual(self.invoke([command], ["--allow-recognition-actions"])[0], 2)
            self.assertEqual(self.invoke([command], ["--allow-recognition-upload"])[0], 0)

    def test_invalid_commands_never_reach_serial(self):
        flags = ["--allow-recognition-actions", "--allow-recognition-upload"]
        for command in ("RECORD 0", "RECORD 61", "RECORD 1\nAT", "RECORD +1", "RESUME ../task",
                        "DISMISS task", "ARCHIVE task 9007199254740992",
                        "ARCHIVE task -1", "ARCHIVE task 1e2", "ARCHIVE task 1.0", "RESUME " + "a" * 65):
            self.assertEqual(self.invoke(["AT+RECOGNITION " + command], flags)[0], 2, command)


if __name__ == "__main__":
    unittest.main()
