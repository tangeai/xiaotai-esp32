from contextlib import redirect_stderr
import io
import json
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import xt
import snapshot
from console_sdk import Vocabulary, _PLATFORM_STATS_KEYS


SHA = "ab" * 32
WDT = "rst:0x8 (TG1WDT_SYS_RESET),boot:0x2b (SPI_FAST_FLASH_BOOT)"
PLATFORM_STATS = ["+PLATFORM-STATS: " + " ".join(f"{key}={int(key == 'login_failures')}"
                                               for key in _PLATFORM_STATS_KEYS[i:i + 14])
                  for i in range(0, len(_PLATFORM_STATS_KEYS), 14)]
DEFAULTS = {
    "AT+NET": ["+NET: connected=0 rssi=-62 ssid_hex=313233 ip=192.0.2.11 mac=02:00:00:00:00:01"],
    "AT+PLATFORM": ["+PLATFORM: online=0 error=0", *PLATFORM_STATS],
    "AT+STATUS state": ["+STATE: started=1 app=companion activity=none uptime_ms=10000"],
    "AT+STATUS framework": ["+FRAMEWORK: owner_stack=6144"],
    "AT+STATUS tasks": ["+TASKS: supported=1 count=2", "+TASK: name=xiaotai stack_min_free=768 priority=4 core=0",
                        "+TASK: name=IDLE stack_min_free=100 priority=0 core=0"],
    "AT+STATUS port": ["+PORT: cache=1"],
    "AT+STATUS net": ["+NET-DIAG: type=link attempts=2 losses=1"],
    "AT+STATUS media": ["+MEDIA: ready=1", "+MEDIA-RX: received=0", "+MEDIA-TX: sent=1 encode_errors=1"],
    "AT+STATUS link": ["+LINK: started=1 connected=1 send_buffer=0 error=0"],
    "AT+MEM": ["+MEM: kind=internal free=20000 min_free=10000", "+MEM-INFO: external=0",
               "+MEM-FAIL: installed=1 error=0 failures=3 size=65536 function_hex=656667"],
    "AT+LOG STATS": ["+LOG-STATS: capacity=64 rejected=0"],
    "AT+LOG": [*[f"+LOG: seq={i} ms={i} level=I src=platform label=ready gen=1 kind=n value=0"
                 for i in range(1, 41)], "+LOG-END: latest=40"],
    "AT+CALL": ["+CALL: kind=device state=idle peer=PRIVATE_PEER token=1467810015 gen=0 error=0"],
    "AT+VIDEO": ["+VIDEO: started=1", "+VIDEO-UP: error=OK phase=off capture_failed=2",
                 "+VIDEO-DOWN: error=OK phase=off"],
}


class FakePort:
    def __init__(self, *, omit=(), overrides=None, reset=None, boot=(), sha=SHA):
        self.commands = []
        self.lines = list(boot)
        self.closed = False
        self.omit, self.overrides, self.reset, self.sha = omit, overrides or {}, reset, sha

    def sdk_exchange(self, command, _wait):
        self.commands.append(command)
        if command == "AT+VERSION":
            return ["+VERSION: board=fixture revision=V3 version=0.1 chip=esp32s3 at=1",
                    f"+BUILD: elf_sha256={self.sha}", "OK"]
        if command == "AT+HELP":
            return ["+HELP: AT", *[f"+HELP: {label}" for label in LABELS if label not in self.omit], "OK"]
        if command == self.reset:
            boot = [WDT, "+READY: at=1 source=esp"]
            self.lines.extend(boot)
            return [*boot, "OK"]
        reply = [*self.overrides.get(command, DEFAULTS[command])]
        if not reply or not reply[-1].startswith("ERROR"):
            reply.append("OK")
        return ["background: password=raw-private", "+BIND: verification=123457", *reply]

    def mark(self):
        return len(self.lines)

    def since(self, at):
        return self.lines[at:]

    def close(self):
        self.closed = True


LABELS = ("AT+NET", "AT+PLATFORM", "AT+STATUS [<section>]", "AT+MEM", "AT+LOG STATS", "AT+LOG [<after>]",
          "AT+LOG FOLLOW 0|1", "AT+CALL", "AT+VIDEO", "AT+BIND")


def vocabulary():
    commands = []
    for label in LABELS:
        name, _, usage = label.partition(" ")
        row = {"command": name, "usage": usage, "access": "read"}
        if label == "AT+LOG STATS":
            row.update(verb="STATS", usage="")
        elif label == "AT+LOG FOLLOW 0|1":
            row.update(verb="FOLLOW", usage="0|1", access="action")
        commands.append(row)
    return Vocabulary({"product": "test-snapshot", "commands": commands}, board="fixture")


class SnapshotTest(unittest.TestCase):
    def setUp(self):
        tmp = TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.output = Path(tmp.name) / "snapshot.json"

    def invoke(self, port, sha=SHA, extra=()):
        args = ["xt.py", "snapshot", "COM7", "--commands", "commands.json", "--elf-sha256", sha,
                "--output", str(self.output), *extra]
        with (patch("sys.argv", args), patch.object(xt, "serial", SimpleNamespace()),
              patch.object(xt, "resolve", return_value=("fixture", {"port": "COM7"})),
              patch.object(xt, "load_vocabulary", return_value=vocabulary()),
              patch.object(xt, "Port", return_value=port) as opened,
              patch.object(xt.subprocess, "run", side_effect=AssertionError("no subprocess")),
              patch("socket.socket", side_effect=AssertionError("no network")), redirect_stderr(io.StringIO())):
            try:
                xt.main()
                code = 0
            except SystemExit as error:
                code = error.code
        if opened.called:
            opened.assert_called_once_with("COM7", exclusive=True)
            self.assertTrue(port.closed)
        return code

    def test_one_session_anomaly_header_bounded_and_sanitized(self):
        port = FakePort(boot=(WDT,))
        self.assertEqual(self.invoke(port), 0)
        raw = self.output.read_text(encoding="utf-8")
        report = json.loads(raw)
        self.assertEqual(next(iter(report)), "summary")
        issues = report["summary"]["issues"]
        for finding in ("platform is offline", "network is disconnected", "MEM-FAIL.failures=3", "PLATFORM-STATS.login_failures=1",
                        "application stack xiaotai minimum 768 B (<1024 B)", "unexpected ROM reset: TG1WDT_SYS_RESET (0x8)",
                        "VIDEO-UP.capture_failed=2"):
            self.assertIn(finding, issues)
        for private in ("PRIVATE_PEER", "raw-private", "123457", "313233", "192.0.2.11", "1467810015", "656667"):
            self.assertNotIn(private, raw)
        log = report["evidence"]["AT+LOG"]["rows"]
        self.assertEqual([row["seq"] for row in log[:-1]], list(range(9, 41)))
        self.assertEqual(report["firmware"]["elf_sha256"], SHA)
        self.assertEqual(port.commands, ["AT+VERSION", "AT+HELP", *snapshot.READS])

    def test_hex_zero_error_is_clean_and_hex_nonzero_is_an_issue(self):
        port = FakePort(overrides={"AT+VIDEO": ["+VIDEO: started=1", "+VIDEO-UP: error=0x0 phase=off",
                                                "+VIDEO-DOWN: error=0x105 phase=off"]})
        self.assertEqual(self.invoke(port), 0)
        issues = json.loads(self.output.read_text())["summary"]["issues"]
        self.assertNotIn("VIDEO-UP.error=0x0", issues)
        self.assertIn("VIDEO-DOWN.error=0x105", issues)
        self.assertEqual([snapshot.reports_error(v) for v in (0, 3, "0", "0x0", "OK", "NONE", "-", "0x105", "TIMEOUT")],
                         [False, True, False, False, False, False, False, True, True])

    def test_unknown_when_capability_or_measurement_missing(self):
        port = FakePort(omit=("AT+VIDEO",), overrides={"AT+LOG STATS": ["+LOG-STATS: capacity=1024"],
                        "AT+STATUS tasks": ["+TASKS: supported=0 count=0"],
                        "AT+STATUS port": ["ERROR: NOT_FOUND SECTION"],
                        "AT+MEM": ["+MEM: kind=internal free=100",
                                   "+MEM-FAIL: installed=0 error=0 failures=0", "+MEM-INFO: external=0"]})
        self.assertEqual(self.invoke(port), 0)
        report = json.loads(self.output.read_text())
        self.assertFalse({"AT+VIDEO", "AT+LOG"} & set(port.commands))
        self.assertTrue({"AT+VIDEO", "AT+LOG", "AT+STATUS port"} <=
                        {name for name, step in report["evidence"].items() if step["status"] == "unknown"})
        for case in ("allocation failure monitoring", "full application task stack coverage is not established by this snapshot",
                     "reset cause: no ROM reset line observed in this capture"):
            self.assertIn(case, report["summary"]["unknown"])

    def test_wrong_identity_never_sends_diagnostics_or_writes_file(self):
        port = FakePort(sha="cd" * 32)
        self.assertEqual(self.invoke(port), 1)
        self.assertEqual(port.commands, ["AT+VERSION"])
        self.assertFalse(self.output.exists())

    def test_restart_stops_writes_preserves_known_and_unknown(self):
        port = FakePort(reset="AT+STATUS state")
        self.assertEqual(self.invoke(port), 0)
        report = json.loads(self.output.read_text())
        self.assertEqual(port.commands[-1], "AT+STATUS state")
        self.assertIn("device restarted during the identified serial session", report["summary"]["issues"])
        self.assertEqual(report["evidence"]["AT+MEM"]["status"], "collected")
        self.assertEqual(report["evidence"]["AT+STATUS tasks"]["status"], "unknown")

    def test_bounded_log_and_partial_sections(self):
        port = FakePort(overrides={"AT+LOG": ["+LOG: seq=41 ms=1 level=I src=platform label=ready gen=0 kind=n value=0",
                                             "+LOG-END: latest=40"], "AT+VIDEO": ["+VIDEO: started=1"]},
                        boot=("rst:0x1 (POWERON),boot:0x2b (SPI_FAST_FLASH_BOOT)",))
        self.assertEqual(self.invoke(port), 0)
        report = json.loads(self.output.read_text())
        self.assertEqual(report["evidence"]["AT+LOG"]["status"], "unknown")
        self.assertEqual(report["evidence"]["AT+VIDEO"]["status"], "unknown")
        self.assertFalse(any("unexpected ROM reset" in value for value in report["summary"]["issues"]))


if __name__ == "__main__":
    unittest.main()
