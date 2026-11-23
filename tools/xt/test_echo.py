import math
import unittest

import xt  # noqa: F401  (puts scripts/ on the import path)
import echo
from console_sdk import ConsoleError, Session, Vocabulary

SHA = "cd" * 32
LABELS = ("AT+STATUS [<section>]", "AT+SET [<name>]", "AT+SET <name> <value>", "AT+FEEDBACK SET <state> 0|1")


def vocabulary():
    rows = [
        {"command": "AT+STATUS", "usage": "[<section>]", "access": "read"},
        {"command": "AT+SET", "usage": "[<name>]", "access": "read"},
        {"command": "AT+SET", "usage": "<name> <value>", "access": "guarded"},
        {"command": "AT+FEEDBACK", "verb": "SET", "usage": "<state> 0|1", "access": "action"},
    ]
    return Vocabulary({"product": "test-echo", "commands": rows}, board="fixture")


class FakeDevice:
    """Speaker echo scales with volume; the processor removes all but 1/20 of it (26 dB)."""

    def __init__(self, levels=True, volume=70):
        self.volume, self.ringing, self.levels, self.reads = volume, False, levels, 0
        self.sent = []

    def __call__(self, command, _wait):
        self.sent.append(command)
        if command == "AT+VERSION":
            return ["+VERSION: board=fixture revision=1 version=0.3.1 chip=esp32s3 at=1",
                    f"+BUILD: elf_sha256={SHA}", "OK"]
        if command == "AT+HELP":
            return ["+HELP: AT", *[f"+HELP: {label}" for label in LABELS], "OK"]
        if command == "AT+SET":
            return [f"+SET: name=audio.volume kind=int value={self.volume} min=0 max=100", "OK"]
        if command.startswith("AT+SET audio.volume "):
            self.volume = int(command.rsplit(" ", 1)[1])
            return ["OK"]
        if command.startswith("AT+FEEDBACK SET companion.incoming "):
            self.ringing = command.endswith(" 1")
            return ["OK"]
        if command == "AT+STATUS media":
            self.reads += 1
            reply = ["+MEDIA: ready=1", "+MEDIA-RX: received=0", "+MEDIA-TX: sent=0"]
            if self.levels:
                echo_level = 50 * self.volume if self.ringing else 0
                mic, ref, processed = 40 + echo_level, (echo_level // 8) or 1, 80 + echo_level // 10
                peak = min(32767, 9 * mic)
                reply.append(f"+MEDIA-LEVELS: window=1 mic_rms={mic} mic_peak={peak} ref_rms={ref} ref_peak={ref * 3}"
                             f" monitor_rms=0 monitor_peak=0 processed_rms={processed} processed_peak={processed * 3}"
                             f" clean_rms={processed * 2} clean_peak={self.reads}")
            return [*reply, "OK"]
        raise AssertionError(command)


class Clock:
    def __init__(self):
        self.now = 0.0

    def sleep(self, seconds):
        self.now += seconds

    def __call__(self):
        return self.now


ALLOW_ALL = ["AT+FEEDBACK SET companion.incoming 1", "AT+FEEDBACK SET companion.incoming 0",
             "AT+SET audio.volume 40", "AT+SET audio.volume 80", "AT+SET audio.volume 70"]


class EchoTest(unittest.TestCase):
    def session(self, device, allow=ALLOW_ALL):
        return Session(device, vocabulary(), allow, SHA)

    def test_measures_each_volume_and_restores(self):
        device, clock = FakeDevice(), Clock()
        report = echo.measure(self.session(device), [40, 80], sleep=clock.sleep, clock=clock)
        self.assertEqual([row["volume"] for row in report["summary"]], [40, 80])
        for row in report["summary"]:
            self.assertTrue(row["window_means"])
            self.assertAlmostEqual(row["echo_reduction_db"], 20 * math.log10(
                row["ring"]["mic_rms"] / 40) - 20 * math.log10(row["ring"]["processed_rms"] / 80), delta=0.2)
            self.assertGreater(row["echo_reduction_db"], 20)
        self.assertFalse(report["summary"][0]["clipped"])
        self.assertTrue(report["summary"][1]["clipped"])  # 9 x mic reaches full scale at volume 80, not at 40
        self.assertEqual(report["volume_before"], 70)
        self.assertEqual(report["volume_restored"], 70)
        self.assertEqual(device.volume, 70)
        self.assertFalse(device.ringing)
        self.assertEqual(device.sent[-2:], ["AT+FEEDBACK SET companion.incoming 0", "AT+SET audio.volume 70"])
        print(echo.table(report["summary"]))

    def test_refuses_before_any_action_when_the_restore_is_not_allowed(self):
        device = FakeDevice()
        allow = [a for a in ALLOW_ALL if a != "AT+SET audio.volume 70"]
        with self.assertRaises(ConsoleError):
            echo.measure(self.session(device, allow), [40, 80], sleep=lambda s: None, clock=Clock())
        self.assertFalse([c for c in device.sent if c.startswith(("AT+SET audio", "AT+FEEDBACK"))])

    def test_keeps_the_volume_when_none_is_given(self):
        device, clock = FakeDevice(volume=55), Clock()
        allow = ALLOW_ALL[:2]
        report = echo.measure(self.session(device, allow), None, sleep=clock.sleep, clock=clock)
        self.assertEqual([row["volume"] for row in report["summary"]], [55])
        self.assertIsNone(report["summary"][0].get("error"))
        self.assertFalse([c for c in device.sent if c.startswith("AT+SET audio")])

    def test_requires_the_level_line(self):
        device = FakeDevice(levels=False)
        with self.assertRaises(ConsoleError):
            echo.measure(self.session(device), [40], sleep=lambda s: None, clock=Clock())
        self.assertFalse([c for c in device.sent if c.startswith(("AT+SET audio", "AT+FEEDBACK"))])


if __name__ == "__main__":
    unittest.main()
