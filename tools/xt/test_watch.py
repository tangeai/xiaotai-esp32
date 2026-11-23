"""Exercise the real watch command with a fake clock and read-only ports."""
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xt


class Disconnected(Exception):
    pass


class WatchFixture:
    def __init__(self):
        self.now = 0.0
        self.sample_times = []
        self.ports = []
        self.lines = []
        self.summary = None
        self.interrupt = False
        self.fail_open_once = False
        self.offline_after = float("inf")
        self.boot_during_sample = float("inf")

    def open(self, _port, _path):
        if self.fail_open_once:
            self.fail_open_once = False
            raise Disconnected("local serial is not present")
        port = SimpleNamespace(mark=lambda: len(self.lines), since=lambda mark: self.lines[mark:], closed=False,
                               sdk_exchange=object())
        port.close = lambda: setattr(port, "closed", True)
        self.ports.append(port)
        return port

    def sleep(self, secs):
        if self.interrupt:
            raise KeyboardInterrupt
        self.now += secs

    def health(self, _session):
        self.sample_times.append(self.now)
        if self.now == self.boot_during_sample:
            self.lines.append("ESP-ROM: fixture")
        return {"online": self.now < self.offline_after, "free": 9000, "owner_stack_1024": True}

    def run(self, *, hours=3 / 3600, every=2):
        args = SimpleNamespace(devs=["fixture"], hours=hours, every=every)

        def finish(_path, summary):
            self.summary = summary

        with (patch.object(xt, "resolve", return_value=("fixture", {"port": "local", "kind": "sdk"})),
              patch.object(xt, "run_dir", return_value=self.temp_dir) as allocate,
              patch.object(xt, "finish", side_effect=finish),
              patch.object(xt, "Port", side_effect=self.open),
              patch.object(xt, "serial", SimpleNamespace(SerialException=Disconnected)),
              patch.object(xt, "sdk_vocabulary", return_value=object()) as vocabulary,
              patch.object(xt, "Session", return_value=object()) as session,
              patch.object(xt.sdk_bench, "health", side_effect=self.health),
              patch.object(xt.time, "monotonic", side_effect=lambda: self.now),
              patch.object(xt.time, "sleep", side_effect=self.sleep)):
            self.allocate = allocate
            self.vocabulary = vocabulary
            self.session = session
            xt.cmd_watch(args)


class WatchTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.fixture = WatchFixture()
        self.fixture.temp_dir = Path(temp.name)

    def test_completes_with_a_boundary_sample_and_no_oversleep(self):
        self.fixture.run(every=10)
        self.assertEqual(self.fixture.sample_times, [0, 3])
        self.assertTrue(self.fixture.ports[0].closed)
        self.assertTrue(self.fixture.summary["window_completed"])
        self.assertTrue(self.fixture.summary["pass"])

    def test_interruption_after_one_healthy_sample_does_not_pass(self):
        self.fixture.interrupt = True
        with self.assertRaises(xt.ConsoleError):
            self.fixture.run()
        self.assertEqual(self.fixture.sample_times, [0])
        self.assertTrue(self.fixture.ports[0].closed)
        self.assertTrue(self.fixture.summary["interrupted"])
        self.assertFalse(self.fixture.summary["window_completed"])
        self.assertFalse(self.fixture.summary["pass"])

    def test_unobserved_sample_before_port_opens_is_not_certified(self):
        self.fixture.fail_open_once = True
        with self.assertRaises(xt.ConsoleError):
            self.fixture.run()
        self.assertEqual(self.fixture.sample_times, [2, 3])
        self.assertTrue(self.fixture.summary["window_completed"])
        self.assertEqual(self.fixture.summary["devices"]["fixture"]["sample_errors"], 1)
        self.assertFalse(self.fixture.summary["pass"])

    def test_drop_at_the_end_is_seen_even_when_the_window_is_shorter_than_interval(self):
        self.fixture.offline_after = 3
        with self.assertRaises(xt.ConsoleError):
            self.fixture.run(every=10)
        self.assertEqual(self.fixture.summary["devices"]["fixture"]["offline_events"], 1)
        self.assertTrue(self.fixture.summary["window_completed"])
        self.assertFalse(self.fixture.summary["pass"])

    def test_boot_in_the_final_reply_window_invalidates_healthy_snapshot(self):
        self.fixture.boot_during_sample = 3
        with self.assertRaises(xt.ConsoleError):
            self.fixture.run()
        self.assertEqual(self.fixture.summary["devices"]["fixture"]["reboots"], 1)
        self.assertTrue(self.fixture.summary["window_completed"])
        self.assertFalse(self.fixture.summary["pass"])

    def test_reboot_rechecks_device_identity_with_the_same_local_vocabulary(self):
        self.fixture.boot_during_sample = 0
        with self.assertRaises(xt.ConsoleError):
            self.fixture.run()
        self.fixture.vocabulary.assert_called_once()
        self.assertEqual(self.fixture.session.call_count, 2)
        first, second = self.fixture.session.call_args_list
        self.assertIs(first.args[1], second.args[1])

    def test_nonfinite_or_nonpositive_window_is_rejected_before_opening(self):
        for inputs in ({"hours": float("nan")}, {"every": 0}, {"every": float("inf")}, {"hours": -1}):
            with self.subTest(inputs=inputs), self.assertRaises(xt.ConsoleError):
                self.fixture.run(**inputs)
            self.fixture.allocate.assert_not_called()
            self.assertEqual(self.fixture.ports, [])

    def test_invalid_local_vocabulary_never_opens_the_port_or_starts_the_window(self):
        args = SimpleNamespace(devs=["fixture"], hours=1, every=2, commands_file=Path("stale-words.json"))
        with (patch.object(xt, "resolve", return_value=("fixture", {"port": "local", "kind": "sdk"})),
              patch.object(xt, "load_vocabulary", side_effect=xt.ConsoleError("wrong checkout")) as load,
              patch.object(xt, "run_dir", side_effect=AssertionError("watch allocated a run")) as allocate,
              patch.object(xt, "Port", side_effect=AssertionError("watch opened a port")) as port):
            with self.assertRaisesRegex(xt.ConsoleError, "wrong checkout"):
                xt.cmd_watch(args)
        load.assert_called_once_with(args.commands_file)
        allocate.assert_not_called()
        port.assert_not_called()


if __name__ == "__main__":
    unittest.main()
