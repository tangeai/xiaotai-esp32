"""SDK-side preservation tests for tests/test_device_console_companion.py."""
# Replaces legacy: tests/test_device_console_companion.py
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

ROOT = Path(__file__).resolve().parents[2]
with patch.object(sys, "path", [str(ROOT / "scripts"), *sys.path]):
    import console_sdk


SHA = "ab" * 32
IDENTITIES = (
    ("xiaotai-waveshare-p4-rev3", "AT+COMPANION", "eh_cp_wifi_sta", "v5.5.4"),
    ("xiaotai-wt99p461-s1-1v1", "AT+COMPANION LIVE", "xiaotai_wt99p461_c61", "v5.5.4"),
)


def identity(project, idf):
    return f"+COMPANION: project={project} version=3.0.7 platform={idf} protocol=3.0.7 elf_sha256={SHA}\r\n".encode()


class CompanionSdkCaptureTest(unittest.TestCase):
    def capture(self, board, query, reply, *, stale=b"", extra=()):
        sends, opens = [], []
        terminal = b"\r\nOK\r\n"
        help_rows = "+HELP: AT" + (f"\r\n+HELP: {query}" if query != "AT" else "")
        replies = {
            "AT+VERSION": (f"+VERSION: board={board} revision=3 version=fixture platform=ESP_IDF chip=esp32p4 cores=2 at=1\r\n"
                           f"+BUILD: elf_sha256={SHA}").encode() + terminal,
            "AT+HELP": help_rows.encode() + terminal,
            query: reply,
            "AT": b"OK\r\n",
        }

        class UART:
            def __init__(self, **_):
                self.pending = bytearray()
                self.stale = stale

            @property
            def in_waiting(self):
                if sends[-1:] == ["AT+HELP"] and not self.pending and self.stale:
                    self.pending.extend(self.stale)
                    self.stale = b""
                return len(self.pending)

            def read(self, count):
                chunk = self.pending[:count]
                del self.pending[:count]
                return bytes(chunk)

            def write(self, data):
                command = data.decode().strip()
                sends.append(command)
                self.pending.extend(replies[command])

            def open(self):
                opens.append((self.dtr, self.rts))

            def flush(self):
                pass

            def close(self):
                pass

        def entry(command, verb=None):
            return {"command": command, "verb": verb, "usage": "", "access": "read"}

        vocab = console_sdk.Vocabulary({"product": board, "commands": [entry("AT"), entry(
            "AT+COMPANION", "LIVE" if query.endswith(" LIVE") else None)]}, board=board)
        with tempfile.TemporaryDirectory() as directory:
            cli = ROOT / "scripts/device_console.py"
            output = Path(directory) / "capture.log"
            argv = [str(cli), "--port", "fixture", "--output", str(output), "--seconds", "1", "--idle-timeout", ".5",
                    "--commands", "fixture-commands.json", "--command", query, *extra]
            tick = iter(i / 10 for i in range(500))
            serial = types.SimpleNamespace(Serial=UART, SerialException=OSError)
            with (patch.dict(sys.modules, {"serial": serial}), patch.object(sys, "argv", argv),
                  patch.object(sys, "path", sys.path.copy()), patch.object(console_sdk, "load_vocabulary", return_value=vocab),
                  patch("time.monotonic", side_effect=lambda: next(tick)), contextlib.redirect_stdout(io.StringIO()),
                  contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as stopped):
                runpy.run_path(str(cli), run_name="__main__")
            if output.exists():
                meta = json.loads(output.with_suffix(".json").read_text(encoding="utf-8"))
                raw = output.read_bytes()
            else:
                meta, raw = None, None
        return stopped.exception.code, sends, opens, meta, raw

    def test_cached_c6_and_live_c61_have_validated_metadata(self):
        for board, query, project, idf in IDENTITIES:
            with self.subTest(query=query):
                row = identity(project, idf)
                if query.endswith(" LIVE"):
                    _, _, words = row.rstrip().partition(b": ")
                    row = b"+COMPANION: " + b" ".join(reversed(words.split())) + b" extra_hex=41\r\n"
                reply = row + b"OK\r\n"
                status, sends, opens, meta, raw = self.capture(board, query, reply)
                self.assertEqual((status, sends, opens), (0, ["AT+VERSION", "AT+HELP", query], [(False, False)]))
                self.assertTrue(raw.endswith(reply))
                self.assertEqual(meta["companion"], {"project": project, "version": "3.0.7", "idf": idf, "elf": SHA})

    def test_only_fresh_complete_unique_identity_is_success(self):
        board, query, project, idf = IDENTITIES[1]
        row = identity(project, idf)
        ok = b"OK\r\n"
        for label, reply, stale in (
            ("bare OK", ok, b""),
            ("no terminal", row, b""),
            ("short hash", row.replace(SHA.encode(), b"abcd") + ok, b""),
            ("duplicate field", row.rstrip() + b" project=second\r\n" + ok, b""),
            ("duplicate", row * 2 + ok, b""),
            ("unterminated", row + b"OK", b""),
            ("error", b"ERROR: NOT_FOUND\r\n", b""),
            ("error then OK", row + b"ERROR: NOT_FOUND\r\n" + ok, b""),
            ("stale identity", ok, row),
            ("stale complete reply", b"", row + ok),
        ):
            with self.subTest(label=label):
                status, sends, _, meta, raw = self.capture(board, query, reply, stale=stale)
                self.assertEqual((status, sends), (1, ["AT+VERSION", "AT+HELP", query]))
                self.assertNotIn("companion", meta)
                self.assertIn("error", meta)
                self.assertIn("companion_error", meta)
                self.assertTrue(raw.endswith(reply))
                if stale:
                    self.assertIn(stale, raw)

    def test_conflicting_capture_is_rejected_before_open(self):
        board, query, project, idf = IDENTITIES[0]
        for extra in (("--command", "AT"), ("--reset-uart",), ("--reset-usb",), ("--idf-open",)):
            with self.subTest(extra=extra):
                self.assertEqual(self.capture(board, query, identity(project, idf) + b"OK\r\n", extra=extra),
                                 (2, [], [], None, None))
        status, sends, opens, meta, _ = self.capture(board, "AT", b"OK\r\n")
        self.assertEqual((status, sends, opens), (0, ["AT+VERSION", "AT+HELP", "AT"], [(False, False)]))
        self.assertNotIn("companion", meta)


if __name__ == "__main__":
    unittest.main()
