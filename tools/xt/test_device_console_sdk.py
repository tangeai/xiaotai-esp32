"""Offline command authorization, wire framing and SDK bench compatibility.

Replaces legacy: tests/test_device_console_environment.py
Replaces legacy: tests/test_device_console_recognition.py
"""
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
sys.path[:0] = [str(ROOT / "scripts"), str(ROOT / "tools/xt")]
from console_sdk import (ConsoleError, ReplyReader, Session, Vocabulary, _PLATFORM_STATS_KEYS,
                         exchange_uart, load_vocabulary, platform_stats)
from console_sdk_aliases import translate
from sdk_selection import apply_variant, load_product, write
import sdk_bench


SHA = "ab" * 32
BOARD = "lckfb-szpi-esp32s3"
VERSION = [f"+VERSION: board={BOARD} revision=3 version=fixture platform=ESP_IDF chip=esp32s3 cores=2 at=1",
           f"+BUILD: elf_sha256={SHA}", "OK"]


def entry(command, verb=None, usage="", access="read", option=None):
    return dict(command=command, verb=verb, usage=usage, access=access, option=option)


ROWS = [entry("AT"), entry("AT+RESTART", access="guarded", option="XIAOTAI_AT_RESTART_COMMAND"),
        entry("AT+SET", usage="[<name>]"), entry("AT+SET", usage="<name> <value>", access="guarded"),
        entry("AT+NET", "PROVISION", "[FORCE]", "action"),
        entry("AT+NET", "PROVISION", "FORGET [FORCE]", "guarded"),
        entry("AT+INTENT", usage="<intent> [<value> [<text-hex>]]", access="guarded")]


def platform_rows():
    rows = ["+PLATFORM: phase=0 online=0"]
    for start, end in ((0, 11), (11, 21), (21, 32), (32, 38)):
        rows.append("+PLATFORM-STATS: " + " ".join(f"{key}=4294967295" for key in _PLATFORM_STATS_KEYS[start:end]))
    return rows


class ConsoleSdkTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        generated = tempfile.TemporaryDirectory()
        cls.addClassCleanup(generated.cleanup)
        cls.vocabulary_file = Path(generated.name) / "commands.json"
        product = apply_variant(load_product(ROOT / "products/xiaotai-szpi-s3/product.json"), "diagnostics")
        write(ROOT, product, cls.vocabulary_file.parent)

    def capture_plan(self, command, *flags, legacy_alias=False, dry_run=True):
        if legacy_alias:
            command, = translate(command, BOARD)
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            log = parent / "no-io" / "capture.log"
            cli = ROOT / "scripts/device_console.py"
            argv = [str(cli), "--port", "fixture", "--output", str(log), "--commands",
                    str(self.vocabulary_file), "--command", command, *flags]
            if dry_run:
                argv.append("--dry-run")
            output = io.StringIO()
            errors = io.StringIO()
            serial = types.SimpleNamespace(Serial=lambda **_: self.fail("preflight opened UART"))
            with (patch.dict(sys.modules, {"serial": serial}), patch.object(sys, "argv", argv),
                  patch.object(sys, "path", sys.path.copy()),
                  contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors),
                  self.assertRaises(SystemExit) as stopped):
                runpy.run_path(str(cli), run_name="__main__")
            self.assertFalse(log.parent.exists())
            self.assertNotIn("secret@", errors.getvalue())
            self.assertNotIn("token=secret", errors.getvalue())
            return stopped.exception.code, output.getvalue()

    def test_environment_console_keeps_reads_and_exact_action_permissions(self):
        for line in ("AT+EP", "AT+EP SHOW", "ep-show"):
            status, output = self.capture_plan(line, legacy_alias=True)
            self.assertEqual(status, 0)
            self.assertEqual(json.loads(output)["commands"], ["AT+EP"])
        for line in ("AT+EP RESET", "ep-reset", "AT+EP CANCEL", "ep-cancel",
                     "AT+EP SET http://fixture.test:8080/pre", "ep-set https://fixture.test/pre"):
            command, = translate(line, BOARD)
            with self.subTest(command=line):
                self.assertEqual(self.capture_plan(line, legacy_alias=True)[0], 2)
                self.assertEqual(self.capture_plan(line, "--allow", "AT+RECOGNITION RECORD 1",
                                                   legacy_alias=True)[0], 2)
                self.assertEqual(self.capture_plan(line, "--allow", command, legacy_alias=True)[0], 0)
        for line in ("AT+RECOGNITION RECORD 1", "AT+BIND CLEAR", "AT+NET FORGET", "AT+RESTART"):
            self.assertEqual(self.capture_plan(line, "--allow", "AT+EP RESET")[0], 2)

    def test_environment_console_rejects_bad_base_before_serial_even_when_allowed(self):
        bad = ("http://", "mqtt://host", "http://user:secret@host", "http://host?token=secret",
               "http://host#part", "http://host:0", "http://host:65536", "http://host:port",
               "http://[::1", "http://host\\path", "http://host path", "http://host\nAT",
               "http://host/" + "a" * 247)
        for line in tuple("AT+EP SET " + base for base in bad) + (
                "AT+EP RESET extra", "AT+EP SET", "AT+EP=DEFAULT", "AT+EP?"):
            with self.subTest(command=line):
                self.assertEqual(self.capture_plan(line, "--allow", line, dry_run=False)[0], 2)

    def test_recognition_console_keeps_read_action_and_upload_forms_separate(self):
        for line in ("AT+RECOGNITION CONFIG", "AT+RECOGNITION STATE"):
            status, output = self.capture_plan(line, legacy_alias=True)
            self.assertEqual(status, 0)
            self.assertTrue(json.loads(output)["dry_run"])
        actions = ("QUERY", "CANCEL", "RESUME task", "DISMISS task item",
                   "ARCHIVE task 0", "ARCHIVE task 9007199254740991")
        for arg in actions + ("RECORD 1", "RECORD 10", "RECORD 60"):
            line = "AT+RECOGNITION " + arg
            other = "AT+RECOGNITION " + ("QUERY" if arg.startswith("RECORD") else "RECORD 1")
            with self.subTest(command=line):
                self.assertEqual(self.capture_plan(line)[0], 2)
                self.assertEqual(self.capture_plan(line, "--allow", other)[0], 2)
                self.assertEqual(self.capture_plan(line, "--allow", line)[0], 0)

    def test_product_vocabularies_keep_the_reviewed_environment_and_recognition_scopes(self):
        for manifest in sorted((ROOT / "products").glob("*/product.json")):
            name = manifest.parent.name
            with self.subTest(product=name), tempfile.TemporaryDirectory() as directory:
                write(ROOT, load_product(manifest), directory)
                vocabulary = load_vocabulary(Path(directory) / "commands.json")
                self.assertEqual(vocabulary.authorize("AT+EP").access, "read")
                for line in ("AT+EP SET http://fixture.test", "AT+EP RESET", "AT+EP CANCEL"):
                    with self.assertRaisesRegex(ConsoleError, "--allow"):
                        vocabulary.authorize(line)
                    vocabulary.authorize(line, {line})
                if name in ("xiaotai-szpi-s3", "xiaotai-cores3-lite", "xiaotai-xmini-c3-v3"):
                    self.assertEqual(vocabulary.authorize("AT+RECOGNITION CONFIG").access, "read")
                    line = "AT+RECOGNITION RECORD 1"
                    if name == "xiaotai-xmini-c3-v3":
                        with self.assertRaisesRegex(ConsoleError, "absent"):
                            vocabulary.authorize(line, {line})
                    else:
                        self.assertEqual(vocabulary.authorize(line, {line}).access, "action")

    def test_recognition_console_rejects_invalid_arguments_before_serial_even_when_allowed(self):
        for arg in ("RECORD 0", "RECORD 61", "RECORD 1\nAT", "RECORD +1", "RESUME ../task",
                    "DISMISS task", "ARCHIVE task 9007199254740992", "ARCHIVE task -1",
                    "ARCHIVE task 1e2", "ARCHIVE task 1.0", "RESUME " + "a" * 65):
            line = "AT+RECOGNITION " + arg
            with self.subTest(command=line):
                self.assertEqual(self.capture_plan(line, "--allow", line, dry_run=False)[0], 2)

    def test_reviewed_environment_and_record_arguments_are_forwarded_unchanged(self):
        vocabulary = load_vocabulary(self.vocabulary_file)
        commands = ("AT+EP SET HTTP://Local-HOST:8080/FiXtUrE", "AT+EP SET https://[::1]:65535/Path",
                    "AT+RECOGNITION RECORD 1", "AT+RECOGNITION RECORD 60")
        forwarded = []

        def exchange(command, _):
            if command == "AT+VERSION":
                return VERSION
            if command == "AT+HELP":
                return ["+HELP: AT", "+HELP: AT+EP SET <base>", "+HELP: AT+RECOGNITION RECORD <seconds:1..60>", "OK"]
            forwarded.append(command)
            return ["OK"]

        session = Session(exchange, vocabulary, commands, SHA)
        for line in commands:
            self.assertEqual(session.send(line), ["OK"])
        self.assertEqual(forwarded, list(commands))

    def test_access_is_per_form_not_verb_or_prefix(self):
        vocabulary = Vocabulary({"product": "fixture", "commands": ROWS}, board=BOARD)
        self.assertEqual(vocabulary.authorize("AT+SET audio.volume").access, "read")
        self.assertEqual(vocabulary.authorize("AT+NET PROVISION FORCE", {"AT+NET PROVISION FORCE"}).access, "action")
        for line, grants in (("AT+SET audio.volume 5", {"AT+SET audio.volume 55"}),
                             ("AT+NET PROVISION FORGET", {"AT+NET PROVISION"}),
                             ("AT+NET PROVISION FORGE", {"AT+NET PROVISION FORGE"}),
                             ("AT+SET audio.volume\r\nAT+RESTART", set()),
                             ("AT+PORT PROBE usb_tick", {"AT+PORT PROBE usb_tick"})):
            with self.subTest(line=line), self.assertRaises(ConsoleError):
                vocabulary.authorize(line, grants)

    def test_running_help_and_fingerprints_required(self):
        requests = []
        vocabulary = Vocabulary({"product": "fixture", "commands": ROWS}, board=BOARD)

        def exchange(line, wait):
            requests.append((line, wait))
            if line == "AT+VERSION":
                return VERSION
            if line == "AT+HELP":
                return ["+HELP: AT", "+HELP: AT+SET [<name>]", "+HELP: AT+SET <name> <value>",
                        "+HELP: AT+INTENT <intent> [<value> [<text-hex>]]", "OK"]
            if line.startswith("AT+INTENT"):
                return ["ERROR: NOT_ALLOWED"]
            return ["OK"]

        with self.assertRaisesRegex(ConsoleError, "fingerprint"):
            Session(exchange, vocabulary, expected_sha="de" * 32)
        no_confirm = Session(exchange, vocabulary, {"AT+SET audio.volume 5"})
        with self.assertRaisesRegex(ConsoleError, "elf-sha256"):
            no_confirm.send("AT+SET audio.volume 5")
        session = Session(exchange, vocabulary, {"AT+SET audio.volume 5", "AT+RESTART"}, SHA)
        with self.assertRaisesRegex(ConsoleError, "does not expose"):
            session.send("AT+RESTART")
        self.assertEqual(session.send("AT+SET audio.volume 5"), ["OK"])
        self.assertEqual(session.send("AT+INTENT binding.forget"), ["ERROR: NOT_ALLOWED"])
        self.assertNotIn("AT+RESTART", [line for line, _ in requests])

    def test_partial_and_long_replies_never_use_a_total_duration(self):
        now = [0]
        reader = ReplyReader(3, lambda: now[0])
        for i in range(25):
            now[0] += 2
            self.assertLess(now[0], reader.deadline)
            reader.receive(f"+TRACE:1,{i}\r\n".encode())
        old_deadline = reader.deadline
        reader.receive(b"I [old log]\r\nO")
        self.assertFalse(reader.done)
        self.assertEqual(reader.deadline, old_deadline)
        reader.receive(b"K\r\n")
        self.assertTrue(reader.done)
        self.assertEqual(reader.lines[-1], "OK")
        with self.assertRaisesRegex(ConsoleError, "multiple"):
            ReplyReader(3).receive(b"OK\r\nERROR\r\n")

    def test_restart_marker_interrupts_an_unfinished_uart_reply(self):
        now = [1]
        reader = ReplyReader(3, lambda: now[0])
        reader.receive(b"+VERSION: board=fixture\r\n+REA")
        deadline = reader.deadline
        now[0] += 2
        with self.assertRaisesRegex(ConsoleError, "device restarted"):
            reader.receive(b"DY: at=1 source=platform locked=0\r\nOK\r\n")
        self.assertFalse(reader.done)
        self.assertEqual(reader.deadline, deadline)

    def test_platform_stats_merges_only_one_complete_snapshot(self):
        keys = _PLATFORM_STATS_KEYS
        self.assertEqual(len(keys), 38)
        rows = platform_rows()
        self.assertEqual([len(row) for row in rows[1:]], [301, 267, 331, 192])
        reader = ReplyReader(3)
        wire = "\r\n".join(rows + ["OK", ""]).encode()
        for i in range(0, len(wire), 19):
            reader.receive(wire[i:i + 19])
        self.assertEqual(platform_stats(reader.lines), dict.fromkeys(keys, 0xffffffff))
        bad = [rows, rows + ["ERROR: FAIL OUTPUT"], rows + ["OK", "OK"], rows[:-1] + ["OK"],
               rows + [rows[1], "OK"], rows[:1] + rows[2:3] + rows[1:2] + rows[3:] + ["OK"],
               rows[:-1] + [rows[-1].replace("4294967295", "4294967296"), "OK"]]
        for malformed in bad:
            with self.assertRaises(ConsoleError):
                platform_stats(malformed)

    def test_sdk_session_checks_only_explicit_platform_reads_and_preserves_text(self):
        rows = platform_rows()
        status_only = ["+NET: connected=1", "OK"]
        replies = {"AT+VERSION": VERSION,
                   "AT+HELP": ["+HELP: AT", "+HELP: AT+PLATFORM", "+HELP: AT+STATUS [<section>]", "OK"],
                   "AT+PLATFORM": rows + ["OK"], "AT+STATUS platform": rows + ["OK"],
                   "AT+STATUS": status_only, "AT+STATUS net": status_only}
        vocabulary = Vocabulary({"product": "fixture", "commands": [entry("AT"), entry("AT+PLATFORM"),
                                  entry("AT+STATUS", usage="[<section>]")]}, board=BOARD)
        session = Session(lambda command, wait: replies[command], vocabulary)
        for command in ("AT+PLATFORM", "AT+STATUS platform"):
            self.assertEqual(session.send(command), rows + ["OK"])
            replies[command] = rows[:-1] + ["OK"]
            with self.assertRaisesRegex(ConsoleError, "platform statistics"):
                session.send(command)
        for command in ("AT+STATUS", "AT+STATUS net"):
            self.assertEqual(session.send(command), status_only)

    def test_off_option_and_tampered_access(self):
        product = apply_variant(load_product(ROOT / "products/xiaotai-szpi-s3/product.json"), "diagnostics")
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "commands.json"
            write(ROOT, product, artifact.parent)
            vocabulary = load_vocabulary(artifact)
            self.assertEqual(vocabulary.board, BOARD)
            with self.assertRaisesRegex(ConsoleError, "does not expose"):
                vocabulary.authorize("AT+SET name 1", {"AT+SET name 1"})
            data = json.loads(artifact.read_text(encoding="utf-8"))
            next(row for row in data["commands"] if row["command"] == "AT+RESTART")["access"] = "read"
            artifact.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ConsoleError, "differs"):
                load_vocabulary(artifact)

    def test_previous_vocabulary_requires_the_firmware_checkout(self):
        data = json.loads(self.vocabulary_file.read_text(encoding="utf-8"))
        rtc = next(row for row in data["commands"] if row["command"] == "AT+MEM" and row["verb"] == "RTC")
        data["commands"].remove(rtc)
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "commands.json"
            artifact.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ConsoleError, "checkout and commands.json from the running firmware's build"):
                load_vocabulary(artifact)

    def test_receive_rejects_stale_or_incomplete_ack(self):
        class Wire:
            pending = bytearray(b"OK\r\n")

            @property
            def in_waiting(self):
                return len(self.pending)

            def read(self, count):
                chunk = self.pending[:count]
                del self.pending[:count]
                return bytes(chunk)

            def write(self, data):
                self.pending.extend(b"OK")

            def flush(self):
                pass

        tick = iter(i / 10 for i in range(30))
        with self.assertRaisesRegex(ConsoleError, "final"):
            exchange_uart(Wire(), "AT", 1, clock=lambda: next(tick))

    def test_reads_named_stack_and_preserves_missing_evidence(self):
        self.assertEqual(sdk_bench.owner_stack(["+TASKS: supported=1 count=4",
            "+TASK: name=at stack_min_free=4096", "+TASK: name=xiaotai stack_min_free=1024"]), 1024)
        self.assertIsNone(sdk_bench.owner_stack(["+TASKS: supported=1 count=50", "+TASK: name=at stack_min_free=4096"]))
        topics = list(sdk_bench.downlink_topics([
            "+TOPIC: src=link name=downlink.rejected kind=1 gen=4 stream=2 codec=3 rate=8000 len=14 n=9 why=1",
            "+TOPIC: src=media name=playout count=5"]))
        self.assertEqual((len(topics), topics[0]["why"], topics[0]["rate"]), (1, 1, 8000))

    def test_conditional_wifi_and_retry_keep_product_behavior(self):
        class State:
            def __init__(self, connected, provisioning, restart):
                self.connected, self.provisioning, self.restart = connected, provisioning, restart

            def send(self, command):
                assert command == "AT+NET"
                return [f"+NET: connected={self.connected} provisioning={self.provisioning}", "OK"]

            def has(self, command):
                return self.restart

        wifi = 'wifi {"ssid":"lab","password":"a"}'
        self.assertEqual(sdk_bench.workflow(State(0, 1, False), wifi)[0], ["AT+NET WIFI 6C6162 61"])
        self.assertEqual(sdk_bench.workflow(State(0, 0, False), wifi)[0], ["AT+NET STORE 6C6162 61", "AT+NET RETRY"])
        self.assertEqual(sdk_bench.workflow(State(1, 0, True), wifi)[0], ["AT+NET STORE 6C6162 61", "AT+RESTART"])
        self.assertIn("power cycle", sdk_bench.workflow(State(1, 0, False), wifi)[1])
        self.assertEqual(sdk_bench.workflow(State(0, 0, False), "AT+RETRY")[0], ["AT+NET RETRY", "AT+BIND RETRY"])
        self.assertEqual(sdk_bench.workflow(State(1, 0, False), "AT+RETRY")[0], ["AT+BIND RETRY"])

    def test_capture_cli_stops_after_timeout_and_legacy_mode_cannot_write_new_firmware(self):
        sent = []
        replies = {"AT+VERSION": VERSION, "AT+HELP": ["+HELP: AT", "+HELP: AT+STATE", "OK"],
                   "AT+STATE": ["+STATE: started=1 state=incomplete"]}

        class UART:
            def __init__(self, **kwargs):
                self.pending = bytearray()

            @property
            def in_waiting(self):
                return len(self.pending)

            def read(self, count):
                chunk = self.pending[:count]
                del self.pending[:count]
                return bytes(chunk)

            def write(self, data):
                command = data.decode().strip()
                sent.append(command)
                self.pending.extend(("\r\n".join(replies[command]) + "\r\n").encode())

            def open(self):
                assert self.dtr is False and self.rts is False

            def flush(self):
                pass

            def close(self):
                pass

        with tempfile.TemporaryDirectory() as directory:
            product = apply_variant(load_product(ROOT / "products/xiaotai-szpi-s3/product.json"), "diagnostics")
            write(ROOT, product, directory)
            cli = ROOT / "scripts/device_console.py"
            for name, flags, written, error, version in (
                ("sdk", ["--commands", str(Path(directory) / "commands.json"), "--command", "AT+STATE",
                         "--command", "AT+STATE"], ["AT+VERSION", "AT+HELP", "AT+STATE"], "timed out", VERSION),
                ("legacy", ["--command", "AT+AI START"], ["AT+VERSION"], "SDK AT firmware detected", VERSION),
                ("locked", ["--legacy", "--command", "AT+AI START"], ["AT+VERSION"], "SDK AT firmware detected",
                 ["ERROR: INVALID_STATE CLI_DISABLED"]),
                ("silent", ["--command", "AT+AI START"], ["AT+VERSION"], "unidentified console", []),
            ):
                with self.subTest(name=name):
                    sent.clear()
                    replies["AT+VERSION"] = version
                    capture = Path(directory) / f"capture-{name}.log"
                    argv = [str(cli), "--port", "fixture", "--output", str(capture), *flags]
                    tick = iter(i / 8 for i in range(500))
                    stub = types.SimpleNamespace(Serial=UART, SerialException=OSError)
                    with (patch.dict(sys.modules, {"serial": stub}), patch.object(sys, "argv", argv),
                          patch("time.monotonic", side_effect=lambda: next(tick)),
                          contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as exit_status):
                        runpy.run_path(str(cli), run_name="__main__")
                    self.assertEqual(exit_status.exception.code, 1)
                    self.assertEqual(sent, written)
                    self.assertIn(error, capture.with_suffix(".json").read_text(encoding="utf-8"))
