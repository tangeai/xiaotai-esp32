"""Exercise the real console CLI with an in-memory UART."""
import contextlib
import io
import json
from pathlib import Path
import runpy
import sys
import tempfile
import types
import unittest
from unittest.mock import patch


CLI = Path(__file__).resolve().parents[1] / "scripts/device_console.py"
SHA = b"e4" * 32
IDENTITY = b"+CPINFO:project=eh_cp_wifi_sta,version=3.0.7,idf=v5.5.4,elf=" + SHA + b"\r\n"


class ConsoleCompanionTests(unittest.TestCase):
    def run_console(self, response=b"", commands=("AT+CPINFO?",), flags=(), before=b""):
        captured = []
        opened = []

        class UART:
            def __init__(self, **kwargs):
                self.pending = bytearray(before)

            def open(self):
                opened.append((self.dtr, self.rts))

            @property
            def in_waiting(self):
                return len(self.pending)

            def read(self, count):
                chunk = bytes(self.pending[:min(count, 32)])
                del self.pending[:len(chunk)]
                return chunk

            def write(self, data):
                captured.append(data)
                self.pending.extend(response)

            def flush(self):
                pass

            def close(self):
                pass

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "uart.log"
            argv = [str(CLI), "--port", "test", "--output", str(output),
                    "--seconds", "6", "--command-delay", "1", *flags]
            for command in commands:
                argv.extend(("--command", command))
            ticks = iter(n * 0.125 for n in range(100))
            status = 0
            with (patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=UART)}),
                  patch.object(sys, "argv", argv),
                  patch("time.monotonic", side_effect=lambda: next(ticks)),
                  contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO())):
                try:
                    runpy.run_path(str(CLI), run_name="__main__")
                except SystemExit as error:
                    status = error.code
            raw = output.read_bytes() if output.exists() else None
            metadata = output.with_suffix(".json")
            result = json.loads(metadata.read_text()) if metadata.exists() else None
        return status, captured, opened, raw, result

    def test_single_read_only_query_reports_complete_identity(self):
        status, commands, opened, raw, metadata = self.run_console(IDENTITY + b"OK\r\n")
        self.assertEqual((status, commands, opened), (0, [b"AT+CPINFO?\r\n"], [(False, False)]))
        self.assertEqual(raw, IDENTITY + b"OK\r\n")
        self.assertEqual(metadata["companion"], {
            "project": "eh_cp_wifi_sta", "version": "3.0.7", "idf": "v5.5.4", "elf": SHA.decode(),
        })

    def test_no_positive_result_for_partial_stale_or_failed_reply(self):
        for label, response, before in (
            ("bare OK", b"OK\r\n", b""),
            ("partial", IDENTITY, b""),
            ("unterminated OK", IDENTITY + b"OK", b""),
            ("short digest", IDENTITY.replace(SHA, b"abcd") + b"OK\r\n", b""),
            ("device failure", b"ERROR,CP_DIAGNOSTIC_FAILED\r\n", b""),
            ("identity followed by failure", IDENTITY + b"ERROR,CP_DIAGNOSTIC_FAILED\r\nOK\r\n", b""),
            ("prior reply", b"", IDENTITY + b"OK\r\n"),
            ("queued prior reply", b"", b"previous device log\r\n" * 15 + IDENTITY + b"OK\r\n"),
            ("duplicate", IDENTITY + IDENTITY + b"OK\r\n", b""),
        ):
            with self.subTest(label=label):
                status, commands, _, raw, metadata = self.run_console(response, before=before)
                self.assertEqual((status, commands), (1, [b"AT+CPINFO?\r\n"]))
                self.assertEqual(raw, before + response)
                self.assertNotIn("companion", metadata)
                self.assertIn("companion_error", metadata)

    def test_conflicting_operations_are_rejected_before_open(self):
        for commands, flags in (
            (("AT+CPINFO?", "AT"), ()),
            (("AT+CPINFO?",), ("--reset-uart",)),
            (("AT+CPINFO?",), ("--reset-usb",)),
            (("AT+CPINFO?",), ("--idf-open",)),
            (("AT+COMPANIONRETRY",), ()),
            (("AT+C6RETRY",), ()),
            (("AT+C61=ROLLBACK",), ()),
        ):
            with self.subTest(commands=commands, flags=flags):
                status, sent, opened, raw, _ = self.run_console(commands=commands, flags=flags)
                self.assertEqual((status, sent, opened, raw), (2, [], [], None))

    def test_existing_capture_keeps_original_result(self):
        status, commands, opened, raw, metadata = self.run_console(b"OK\r\n", commands=("AT",))
        self.assertEqual((status, commands, opened, raw), (0, [b"AT\r\n"], [(False, False)], b"OK\r\n"))
        self.assertNotIn("companion", metadata)
        self.assertNotIn("companion_error", metadata)


if __name__ == "__main__":
    unittest.main()
