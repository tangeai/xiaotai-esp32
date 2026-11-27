from contextlib import redirect_stderr, redirect_stdout
import io
import json
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import xt
import app_stacks
from console_sdk import ConsoleError, Vocabulary, _PLATFORM_STATS_KEYS


SHA = "ac" * 32
SCOPE = "+PORT: tasks_scope=application_subset unit=bytes"


def task(name, free, *, core=0):
    return f"+TASK: name={name} stack_min_free={free} priority=3 core={core}"


def table(*rows, supported=1, count=None, error=None):
    tail = f" error={error}" if error is not None else ""
    return [f"+TASKS: supported={supported} count={len(rows) if count is None else count}{tail}", *rows, "OK"]


def platform(free):
    stats = [f"{key}={free if key == 'worker_stack_free' else 0}" for key in _PLATFORM_STATS_KEYS]
    return ["+PLATFORM: online=1", *("+PLATFORM-STATS: " + " ".join(stats[i:i + 12])
                                   for i in range(0, len(stats), 12)), "OK"]


class FakePort:
    def __init__(self, lines, *, board="test-board", fingerprint=SHA, status_help=True, subset=None, workers=None):
        self.lines = lines
        self.subset = ["ERROR: NOT_FOUND SECTION"] if subset is None else subset
        self.board = board
        self.fingerprint = fingerprint
        self.status_help = status_help
        self.workers = workers or {}
        self.commands = []
        self.closed = False

    def sdk_exchange(self, command, _wait):
        self.commands.append(command)
        if command == "AT+VERSION":
            return [f"+VERSION: at=1 board={self.board} chip=esp32s3", f"+BUILD: elf_sha256={self.fingerprint}", "OK"]
        if command == "AT+HELP":
            return ["+HELP: AT", *(["+HELP: AT+STATUS [<section>]"] if self.status_help else []),
                    *(f"+HELP: {name}" for name in self.workers), "OK"]
        if command == "AT+STATUS tasks":
            return self.lines
        if command == "AT+STATUS port":
            return self.subset
        if command in self.workers:
            return self.workers[command]
        raise AssertionError("unexpected non-read command")

    def close(self):
        self.closed = True


class AppStacksTest(unittest.TestCase):
    def score(self, lines, expected, minimum=1024):
        status, count, rows = app_stacks.parse_snapshot(lines)
        return app_stacks.report(status, count, rows, expected, minimum)

    def test_threshold_legitimate_zero_and_explicit_scope(self):
        lines = table(task("network", 1100), task("idle0", 20), task("xiaotai", 1024, core=-1))
        result = self.score(lines, ["xiaotai", "network"])
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual(result["selected_min_free_bytes"], 1024)
        self.assertEqual(result["observed_min_free_bytes"], 20)
        self.assertEqual(result["all_application_tasks"], "not_measured")
        self.assertEqual(self.score(lines, ["xiaotai"], 1025)["below_threshold"], ["xiaotai"])
        result = self.score(table(task("xiaotai", 0)), ["xiaotai"])
        self.assertEqual(result["verdict"], "fail")
        self.assertEqual(result["selected_min_free_bytes"], 0)

    def test_inventory_and_missing_tasks_are_not_measured(self):
        lines = table(task("nvs", 2048))
        self.assertEqual(self.score(lines, [])["verdict"], "not_measured")
        result = self.score(lines, ["nvs", "xt_ui"])
        self.assertEqual(result["verdict"], "not_measured")
        self.assertEqual(result["missing"], ["xt_ui"])
        self.assertFalse(result["selected_coverage_complete"])
        result = self.score(table(task("nvs", 512)), ["nvs", "xt_ui"])
        self.assertEqual(result["verdict"], "fail")
        self.assertEqual(result["missing"], ["xt_ui"])

    def test_duplicate_task_identity_does_not_pick_one(self):
        rows = table(task("xt_ui", 1024, core=0), task("xt_ui", 1500, core=1), task("xiaotai", 2048))
        self.assertEqual(self.score(rows, ["xiaotai"])["verdict"], "pass")
        result = self.score(rows, ["xt_ui"])
        self.assertEqual(result["verdict"], "not_measured")
        self.assertEqual(result["ambiguous"], ["xt_ui"])
        self.assertIsNone(result["selected_min_free_bytes"])

    def test_trace_off_provider_error_and_truncation(self):
        for lines, kind in ((table(supported=0), "unsupported"), (table(error="no_mem"), "provider_error"),
                            (table(), "provider_error"), (table(task("xiaotai", 999), count=49), "incomplete")):
            with self.subTest(kind=kind):
                result = self.score(lines, ["xiaotai"])
                self.assertEqual(result["verdict"], "not_measured")
                self.assertEqual(result["table_status"], kind)
                self.assertEqual(result["below_threshold"], [])
        self.assertEqual(self.score(table(task("xiaotai", 999), count=49), ["xiaotai"])["observed_min_free_bytes"], 999)

    def test_malformed_or_unframed_replies_get_no_watermark(self):
        good = table(task("xiaotai", 1024))
        bad = (good[:-1], [*good, "OK"], [good[0], *good], table(task("xiaotai", "-1")),
               table(task("xiaotai", 1 << 32)), table(task("xiaotai", 20), supported=0),
               table(task("xiaotai", 20, core="nor")), ["ERROR: BUSY"])
        for lines in bad:
            with self.subTest(lines=lines), self.assertRaises(ConsoleError):
                app_stacks.parse_snapshot(lines)

    def test_cli_checks_identity_and_only_reads_status(self):
        for port, code, status in (
                (FakePort(table(task("xiaotai", 1024))), 0, "complete"),
                (FakePort(table(supported=0)), 1, "unsupported"),
                (FakePort(table(supported=0), subset=["+PORT: cache=0", "OK"]), 1, "unsupported"),
                (FakePort(["ERROR: BUSY"]), 1, "reply_failed"),
                (FakePort(table(), status_help=False), 1, "command_unavailable"),
                (FakePort(table(), fingerprint="de" * 32), 1, None),
                (FakePort(table(), board="different"), 1, None)):
            with self.subTest(status=status):
                actual, body = self.run_cli(port)
                self.assertEqual(actual, code)
                self.assertTrue(port.closed)
                if status is None:
                    self.assertEqual(port.commands, ["AT+VERSION"])
                    self.assertFalse(body)
                else:
                    expected = ["AT+VERSION", "AT+HELP"]
                    if status != "command_unavailable":
                        expected.append("AT+STATUS tasks")
                    if status == "unsupported":
                        expected.append("AT+STATUS port")
                    self.assertEqual(port.commands, expected)
                    self.assertEqual(json.loads(body)["table_status"], status)

    def test_subset_fallback_scores_only_real_samples(self):
        partial = [SCOPE, "+PORT: task=xt_ui state=unavailable stack_min_free=- lookup_us=1",
                   "+PORT: task=xiaotai state=sampled stack_min_free=1024 lookup_us=1", "OK"]
        port = FakePort(table(supported=0), subset=partial)
        code, body = self.run_cli(port)
        self.assertEqual(code, 0)
        result = json.loads(body)
        self.assertEqual(result["source"], "port_application_subset")
        self.assertIsNone(result["declared_count"])
        self.assertEqual(result["selected_min_free_bytes"], 1024)
        self.assertEqual(port.commands, ["AT+VERSION", "AT+HELP", "AT+STATUS tasks", "AT+STATUS port"])
        rows = app_stacks.parse_application_subset(partial)
        result = app_stacks.report("application_subset", None, rows, ["xt_ui"], 1024, "port_application_subset")
        self.assertEqual(result["unsampled"], ["xt_ui"])
        self.assertEqual(result["verdict"], "not_measured")
        zero = [line.replace("stack_min_free=1024", "stack_min_free=0") for line in partial]
        code, body = self.run_cli(FakePort(table(supported=0), subset=zero))
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(body)["verdict"], "fail")

    def test_subset_requires_scope_and_complete_reply(self):
        row = "+PORT: task=xiaotai state=sampled stack_min_free=1500 lookup_us=1"
        for lines in ([row, "OK"], [SCOPE, SCOPE, row, "OK"], [SCOPE.replace("bytes", "words"), row, "OK"],
                      [SCOPE, row], [SCOPE, row.replace("sampled", "unavailable"), "OK"]):
            with self.subTest(lines=lines):
                code, body = self.run_cli(FakePort(table(supported=0), subset=lines))
                self.assertEqual(code, 1)
                self.assertEqual(json.loads(body)["verdict"], "not_measured")

    def test_invalid_cli_request_stops_before_opening(self):
        for opts in (("--elf-sha256", "ac"), ("--min-free", "0"), ("--expect-task", "xiaotai")):
            binary = FakePort([])
            self.assertEqual(self.run_cli(binary, extra=opts, opens=False), (1, ""))
            self.assertFalse(binary.commands or binary.closed)

    def test_worker_reports_do_not_change_task_inventory_verdict(self):
        replies = {"AT+LINK": ["+LINK: started=1 worker_stack_free=512", "OK"], "AT+PLATFORM": platform(1808)}
        port = FakePort(table(task("xiaotai", 1024)), workers=replies)
        code, body = self.run_cli(port)
        self.assertEqual(code, 0)
        self.assertNotIn("worker_watermarks", json.loads(body))
        self.assertNotIn("AT+LINK", port.commands)
        port = FakePort(table(task("xiaotai", 1024)), workers=replies)
        code, body = self.run_cli(port, extra=("--workers",))
        self.assertEqual(code, 0)
        result = json.loads(body)
        self.assertEqual(result["selected_min_free_bytes"], 1024)
        self.assertEqual(result["worker_watermarks"], {
            "transport": {"command": "AT+LINK", "status": "below_threshold", "stack_min_free_bytes": 512},
            "platform": {"command": "AT+PLATFORM", "status": "sampled", "stack_min_free_bytes": 1808}})
        self.assertEqual(port.commands, ["AT+VERSION", "AT+HELP", "AT+STATUS tasks", "AT+LINK", "AT+PLATFORM"])

    def test_workers_are_separate_when_task_table_is_unsupported(self):
        for link, expected_calls in ((None, ["AT+PLATFORM"]),
                (["+LINK: started=1 worker_stack_free=0", "OK"], ["AT+LINK", "AT+PLATFORM"]),
                (["+LINK: worker_stack_free=4294967296", "OK"], ["AT+LINK", "AT+PLATFORM"]),
                (["+LINK: worker_stack_free=100 worker_stack_free=200", "OK"], ["AT+LINK", "AT+PLATFORM"]),
                (["+LINK: worker_stack_free=1500"], ["AT+LINK"])):
            with self.subTest(link=link):
                replies = {"AT+PLATFORM": platform(0)}
                if link is not None:
                    replies["AT+LINK"] = link
                port = FakePort(table(supported=0), workers=replies)
                code, body = self.run_cli(port, extra=("--workers",))
                self.assertEqual(code, 1)
                evidence = json.loads(body)["worker_watermarks"]
                self.assertEqual(port.commands[-len(expected_calls):], expected_calls)
                for sample in evidence.values():
                    self.assertEqual((sample["status"], sample["stack_min_free_bytes"]), ("not_measured", None))

    def run_cli(self, port, extra=(), *, opens=True):
        argv = ["xt.py", "app-stacks", "PORT", "--commands", "commands.json", "--elf-sha256", SHA,
                "--expect-task", "xiaotai", *extra]
        vocab = Vocabulary({"product": "sample", "commands": [
            {"command": "AT+STATUS", "usage": "[<section>]", "access": "read"},
            {"command": "AT+LINK", "usage": "", "access": "read"},
            {"command": "AT+PLATFORM", "usage": "", "access": "read"}]},
            board="test-board")
        with (patch("sys.argv", argv), patch.object(xt, "serial", SimpleNamespace()),
              patch.object(xt, "resolve", return_value=("fixture", {"port": "fixture"})),
              patch.object(xt, "load_vocabulary", return_value=vocab), patch.object(xt, "Port", return_value=port) as opened,
              patch.object(xt.subprocess, "run", side_effect=AssertionError("no subprocess")),
              redirect_stdout(io.StringIO()) as out, redirect_stderr(io.StringIO())):
            code = 0
            try:
                xt.main()
            except SystemExit as stop:
                code = stop.code
        if opens:
            opened.assert_called_once_with("fixture", exclusive=True)
        else:
            opened.assert_not_called()
        return code, out.getvalue()


if __name__ == "__main__":
    unittest.main()
