"""Exercise the real SDK soak entry with a serial transcript and fake clock."""
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xt

from console_sdk import Vocabulary
from sdk_soak import SETTLE_SECONDS


SHA = "ac" * 32


class Board:
    def __init__(self):
        entries = [dict(command=x, access="read", usage="")
                   for x in ("AT", "AT+VERSION", "AT+STATE", "AT+LINK", "AT+AI", "AT+MEM")]
        entries += [dict(command="AT+AI", verb=x, access="action", usage="") for x in ("START", "STOP")]
        self.vocab = Vocabulary({"product": "host-soak", "commands": entries}, board="fixture")
        self.now = 0.0
        self.boot_at = -10.0
        self.log = []
        self.quiet = []
        self.calls = []
        self.round = 0
        self.active = False
        self.summary = None
        self.on_settle = lambda count: None
        self.sha = SHA
        self.idle_loss_per_round = 5
        self.idle_loss_extra = 0
        self.rx = 4
        self.bad_memory = False
        self.foreign_generation = False

    def sleep(self, seconds):
        if seconds == SETTLE_SECONDS:
            self.quiet.append(len(self.calls))
            self.on_settle(len(self.quiet))
        self.now += seconds

    def mark(self):
        return len(self.log)

    def since(self, mark):
        return self.log[mark:]

    def close(self):
        pass

    def sdk_exchange(self, command, wait):
        self.calls.append(command)
        self.now += .02
        if command == "AT+VERSION":
            return ["+VERSION: board=fixture revision=1 chip=esp32s3 at=1", f"+BUILD: elf_sha256={self.sha}", "OK"]
        if command == "AT+HELP":
            return [f"+HELP: {entry.label}" for entry, _ in self.vocab.entries] + ["OK"]
        if command == "AT+STATE":
            activity, active_id = ("ai", self.round) if self.active else ("none", 0)
            uptime = int((self.now - self.boot_at) * 1000)
            return [f"+STATE: started=1 app=fixture activity={activity} id={active_id} uptime_ms={uptime}", "OK"]
        if command == "AT+AI":
            phase = "listening" if self.active else "idle"
            generation = self.round + int(self.foreign_generation and self.active and self.now > 134)
            return [f"+AI: phase={phase} gen={generation}", "OK"]
        if command == "AT+LINK":
            return [f"+LINK-MEDIA: tx={self.round * 12} rx={self.round * self.rx} tx_dropped=0 rx_dropped=0 stale=0", "OK"]
        if command == "AT+MEM":
            free = 10000 - self.round * self.idle_loss_per_round - (self.idle_loss_extra if self.round else 0)
            largest = 7000 - self.round * 29
            rows = [f"+MEM: kind={kind} total=15000 free={free} min_free=7000 largest={largest} min_largest=6000 samples=1"
                    for kind in ("internal", "bulk", "dma")]
            return rows[:-1] + ["OK"] if self.bad_memory else rows + ["+MEM-INFO: external=0", "OK"]
        if command == "AT+AI START":
            self.active = True
            self.round += 1
            return ["OK"]
        if command == "AT+AI STOP":
            self.active = False
            return ["OK"]
        raise AssertionError(command)

    def capture(self, _, summary):
        self.summary = summary

    def run(self, *, permits=("AT+AI START", "AT+AI STOP"), rounds=2):
        args = SimpleNamespace(dev="fixture", peer=None, rounds=rounds, secs=8,
                               commands_file="fixture-commands.json", elf_sha256=SHA, allow=permits)
        with (patch.object(xt, "Port", return_value=self), patch.object(xt, "resolve", return_value=("fixture", {"port": "mock"})),
              patch.object(xt, "run_dir", return_value=Path("unused")), patch.object(xt, "finish", side_effect=self.capture),
              patch.object(xt, "load_vocabulary", return_value=self.vocab), patch.object(xt.time, "sleep", side_effect=self.sleep),
              patch.object(xt.time, "monotonic", side_effect=lambda: self.now)):
            xt.cmd_soak(args)


class SdkSoakTest(unittest.TestCase):
    def test_completed_run_brackets_and_compares_two_passive_samples(self):
        board = Board()
        board.run()
        report = board.summary
        self.assertTrue(report["pass"] and report["same_boot_verified"])
        self.assertEqual(report["requested_rounds"], 2)
        self.assertGreaterEqual(report["first"]["idle_seconds"], 130)
        self.assertGreaterEqual(report["last"]["idle_seconds"], 130)
        self.assertEqual([board.calls[i] for i in board.quiet], ["AT+VERSION", "AT+VERSION"])
        self.assertLess(board.quiet[0], board.calls.index("AT+AI START"))
        self.assertGreater(board.quiet[1], len(board.calls) - 1 - board.calls[::-1].index("AT+AI STOP"))
        self.assertEqual(board.calls.count("AT+MEM"), 2)
        self.assertEqual([row["after"]["elf_sha256"] for row in report["rounds"]], [SHA, SHA])
        self.assertEqual(report["free_delta_bytes"]["internal"], -10)
        self.assertEqual(report["largest_delta_bytes"]["internal"], -58)
        self.assertEqual(report["idle_free_gate"], {"status": "measured_only"})

    def test_wrong_final_firmware_refuses_pass_even_after_all_rounds(self):
        board = Board()
        board.on_settle = lambda count: setattr(board, "sha", "bc" * 32) if count == 2 else None
        with self.assertRaises(xt.ConsoleError):
            board.run()
        self.assertEqual(len(board.summary["rounds"]), 2)
        self.assertFalse(board.summary["same_boot_verified"])
        self.assertNotIn("free_delta_bytes", board.summary)
        self.assertEqual(board.summary["idle_free_gate"], {"status": "not_verified"})

    def test_same_sha_reboot_with_increased_uptime_cannot_pass(self):
        board = Board()
        board.on_settle = lambda count: setattr(board, "boot_at", board.now + 0.1) if count == 1 else None
        with self.assertRaises(xt.ConsoleError):
            board.run()
        self.assertIn("continuous device uptime", board.summary["failure"])
        self.assertNotIn("AT+AI START", board.calls)

    def test_boot_mark_in_quiet_window_invalidates_evidence(self):
        board = Board()
        board.on_settle = lambda count: board.log.append("ESP-ROM: fixture") if count == 2 else None
        with self.assertRaises(xt.ConsoleError):
            board.run()
        self.assertIn("boot marker", board.summary["failure"])

    def test_missing_stop_consent_or_memory_never_starts(self):
        for bad_memory in (False, True):
            with self.subTest(bad_memory=bad_memory):
                board = Board()
                board.bad_memory = bad_memory
                permits = ("AT+AI START", "AT+AI STOP") if bad_memory else ("AT+AI START",)
                with self.assertRaises(xt.ConsoleError):
                    board.run(permits=permits)
                self.assertNotIn("AT+AI START", board.calls)
                self.assertFalse(board.summary["pass"])

    def test_foreign_generation_is_not_stopped_and_round_is_incomplete(self):
        board = Board()
        board.foreign_generation = True
        with self.assertRaises(xt.ConsoleError):
            board.run()
        self.assertNotIn("AT+AI STOP", board.calls)
        self.assertFalse(board.summary["pass"])
        self.assertEqual(board.summary["rounds"], [])

    def test_missing_downlink_never_passes_even_with_comparable_heap(self):
        board = Board()
        board.rx = 0
        with self.assertRaises(xt.ConsoleError):
            board.run()
        self.assertTrue(board.summary["same_boot_verified"])
        self.assertFalse(board.summary["pass"])
        self.assertEqual(len(board.summary["rounds"]), 2)

    def test_ten_round_acceptance_rejects_257_byte_settled_loss_with_good_media(self):
        board = Board()
        board.idle_loss_per_round, board.idle_loss_extra = 25, 7
        with self.assertRaises(xt.ConsoleError):
            board.run(rounds=10)
        self.assertTrue(board.summary["same_boot_verified"])
        self.assertEqual(len(board.summary["rounds"]), 10)
        self.assertEqual(board.summary["free_delta_bytes"]["internal"], -257)
        self.assertEqual(board.summary["idle_free_gate"], {"status": "fail", "max_loss_bytes": 256})
        self.assertFalse(board.summary["pass"])

    def test_ten_round_acceptance_allows_256_and_keeps_largest_separate(self):
        board = Board()
        board.idle_loss_per_round, board.idle_loss_extra = 25, 6
        board.run(rounds=10)
        self.assertEqual(board.summary["free_delta_bytes"]["internal"], -256)
        self.assertEqual(board.summary["largest_delta_bytes"]["internal"], -290)
        self.assertEqual(board.summary["idle_free_gate"], {"status": "pass", "max_loss_bytes": 256})
        self.assertTrue(board.summary["pass"])

    def test_other_round_counts_report_memory_without_applying_ten_round_limit(self):
        board = Board()
        board.idle_loss_extra = 300
        board.run(rounds=2)
        self.assertEqual(board.summary["free_delta_bytes"]["internal"], -310)
        self.assertEqual(board.summary["idle_free_gate"], {"status": "measured_only"})
        self.assertTrue(board.summary["pass"])
