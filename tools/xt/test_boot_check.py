from contextlib import contextmanager, redirect_stderr, redirect_stdout
import io
import json
from threading import Event, Lock
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import xt
import boot_check
from console_sdk import ConsoleError, Vocabulary, _PLATFORM_STATS_KEYS


SHA = "ac" * 32
USB_BANNER = ("ESP-ROM:esp32s3-20210327", "Build:Mar 27 2021",
              "rst:0x15 (USB_UART_CHIP_RESET),boot:0x2b (SPI_FAST_FLASH_BOOT)")
C3_USB_BANNER = ("ESP-ROM:esp32c3-api1-20210207", "Build:Feb  7 2021",
                 "rst:0x15 (USB_UART_CHIP_RESET),boot:0x8 (SPI_FAST_FLASH_BOOT)")
STATE = "+STATE: started=1 app={app} activity={activity} id={id} uptime_ms={uptime}"
ONLINE = "+PLATFORM: phase=6 online=1 clock=1 discovered=1 authorized=1 signaling=1 verifying=0 held=0 gated=0"
STATS = ["+PLATFORM-STATS: " + " ".join(f"{key}=0" for key in _PLATFORM_STATS_KEYS[start:start + 8])
         for start in range(0, len(_PLATFORM_STATS_KEYS), 8)]
ALLOWED = {"AT+VERSION", "AT+HELP", "AT+STATE", "AT+NET", "AT+PLATFORM"}


def vocabulary():
    rows = [{"command": name, "usage": "", "access": "read"} for name in sorted(ALLOWED)]
    return Vocabulary({"product": "test-board", "commands": rows}, board="test-board")


class Bench:
    def __init__(self):
        self.now = 100.0
        self.epoch = 40.0
        self.pre_version = self.post_version = SHA
        self.board = "test-board"
        self.chip = "esp32s3"
        self.post_chip = None
        self.blocked = False
        self.busy = False
        self.change_uptime = True
        self.uptime_restart_on_net = False
        self.before = [*USB_BANNER, "+READY: at=1 source=fixture locked=0"]
        self.new_lines = USB_BANNER
        self.replies = []
        self.commands = []
        self.reset_attempts = 0
        self.opens = 0
        self.closes = 0
        self.network = "1"
        self.address = "192.0.2.9"
        self.online = "1"
        self.lose_usb = False
        self.clock = lambda: self.now

    def sleep(self, seconds):
        self.now += seconds

    @contextmanager
    def open_port(self):
        self.opens += 1
        self.replies = self.before.copy()
        try:
            yield self
        finally:
            self.closes += 1

    def since(self, mark):
        return self.replies[mark:]

    def confirm(self):
        if self.lose_usb and self.reset_attempts:
            raise ConsoleError("USB port identity changed")

    def reset(self, port):
        if port is not self:
            raise AssertionError("unexpected port")
        mark = len(self.replies)
        started = self.now
        self.reset_attempts += 1
        if self.change_uptime:
            self.epoch = started + 0.01
        self.sleep(0.3)
        self.replies.extend(self.new_lines)
        return mark, started

    def sdk_exchange(self, command, wait):
        if command not in ALLOWED:
            raise AssertionError(f"unsafe command {command}")
        self.commands.append(command)
        self.sleep(0.04)
        if command == "AT+VERSION":
            if self.blocked:
                return ["ERROR: INVALID_STATE CLI_DISABLED"]
            fingerprint = self.pre_version if not self.reset_attempts else self.post_version
            chip = self.post_chip if self.reset_attempts and self.post_chip else self.chip
            return [f"+VERSION: board={self.board} chip={chip} at=1", f"+BUILD: elf_sha256={fingerprint}", "OK"]
        if command == "AT+HELP":
            return [*(f"+HELP: {key}" for key in sorted({*ALLOWED, "AT"})), "OK"]
        if command == "AT+STATE":
            return [STATE.format(app="companion", activity="ai_talk" if self.busy else "none",
                                 id=13 if self.busy else 0, uptime=int((self.now - self.epoch) * 1000)), "OK"]
        if command == "AT+NET":
            if self.uptime_restart_on_net:
                self.epoch = self.now
            return [f"+NET: state=up connected={self.network} held=0 provisioning=0 ip={self.address}"
                    " ssid_hex=736563726574 mac=00:11:22:33:44:55", "OK"]
        return [ONLINE.replace("online=1", f"online={self.online}"), *STATS, "OK"]

    def run(self, *, rounds=2, app="companion"):
        return boot_check.run(self.open_port, self.reset, self.confirm, vocabulary(), SHA, app, rounds, 10,
                              clock=self.clock, sleep=self.sleep)


class BootCheckTest(unittest.TestCase):
    def test_all_rounds_use_fresh_rom_and_exact_sessions(self):
        bench = Bench()
        result = bench.run(rounds=3)
        self.assertTrue(result["pass"], result)
        self.assertFalse(result["cold_boot"])
        self.assertEqual((bench.opens, bench.closes, bench.reset_attempts), (3, 3, 3))
        self.assertEqual(bench.commands.count("AT+VERSION"), 6)
        self.assertTrue(all(row["rom_reset"] and row["boot"] and row["platform"] for row in result["rounds"]))
        self.assertNotRegex(json.dumps(result), r"736563726574|192\.0\.2\.9|00:11:22:33:44:55")

    def test_c3_native_usb_requires_the_identified_rom(self):
        bench = Bench()
        bench.chip = "esp32c3"
        bench.before = [*C3_USB_BANNER]
        bench.new_lines = C3_USB_BANNER
        result = bench.run(rounds=3)
        self.assertTrue(result["pass"], result)
        self.assertEqual(bench.reset_attempts, 3)

        for log in (USB_BANNER, (USB_BANNER[0], *C3_USB_BANNER)):
            with self.subTest(log=log):
                bench = Bench()
                bench.chip = "esp32c3"
                bench.new_lines = log
                result = bench.run()
                self.assertFalse(result["pass"])
                self.assertEqual(result["rounds"][0]["stop_stage"], "rom")
                self.assertEqual(bench.reset_attempts, 1)

    def test_post_reset_chip_must_match_even_when_board_and_elf_match(self):
        bench = Bench()
        bench.post_chip = "esp32c3"
        result = bench.run()
        self.assertFalse(result["pass"])
        self.assertEqual(result["rounds"][0]["stop_stage"], "identity")
        self.assertIn("running chip changed", result["rounds"][0]["error"])
        self.assertEqual(bench.reset_attempts, 1)

    def test_fail_closed_before_reset(self):
        for overrides in ({"pre_version": "de" * 32}, {"pre_version": "-"}, {"blocked": True}, {"busy": True},
                          {"board": "other-board"}, {"chip": "esp32p4"}):
            bench = Bench()
            with self.subTest(overrides=overrides):
                vars(bench).update(overrides)
                result = bench.run()
                self.assertFalse(result["pass"])
                self.assertEqual(bench.reset_attempts, 0)
                self.assertFalse(result["rounds"][0]["attempted"])
        bench = Bench()
        self.assertFalse(bench.run(app="other")["pass"])
        self.assertEqual(bench.reset_attempts, 0)

    def test_stale_rom_wrong_cause_extra_boot_or_no_reset(self):
        changes = ({"new_lines": ("+READY: at=1 source=fixture locked=0",)},
                   {"new_lines": (USB_BANNER[0], "rst:0x1 (POWERON),boot:0x2b (SPI_FAST_FLASH_BOOT)")},
                   {"new_lines": USB_BANNER + USB_BANNER},
                   {"new_lines": USB_BANNER + ("Guru Meditation Error",)},
                   {"change_uptime": False})
        for overrides in changes:
            bench = Bench()
            with self.subTest(overrides=overrides):
                vars(bench).update(overrides)
                result = bench.run()
                self.assertFalse(result["pass"])
                self.assertFalse(result["rounds"][0]["boot"])
                self.assertEqual(bench.reset_attempts, 1)

    def test_new_identity_and_same_boot_are_mandatory(self):
        for overrides in ({"post_version": "de" * 32}, {"lose_usb": True}, {"uptime_restart_on_net": True}):
            bench = Bench()
            with self.subTest(overrides=overrides):
                vars(bench).update(overrides)
                self.assertFalse(bench.run()["pass"])
                self.assertEqual((bench.opens, bench.closes, bench.reset_attempts), (1, 1, 1))

    def test_online_layers_are_not_interchangeable(self):
        for overrides, expected in (({"address": "0.0.0.0"}, (False, True)),
                                    ({"network": "0"}, (False, True)), ({"online": "0"}, (True, False))):
            bench = Bench()
            with self.subTest(overrides=overrides):
                vars(bench).update(overrides)
                result = bench.run()
                self.assertFalse(result["pass"])
                row = result["rounds"][0]
                self.assertTrue(row["boot"] and row["app"])
                self.assertEqual((row["network"], row["platform"]), expected)
                self.assertEqual(bench.reset_attempts, 1)

    def test_usb_requires_unique_descriptor_and_explicit_port(self):
        device = SimpleNamespace(device="COM25", vid=0x303A, pid=0x1001, serial_number="TEST:12")
        bridge = SimpleNamespace(device="COM27", vid=0x1A86, pid=0x55D3, serial_number="TEST:12")
        self.assertEqual(boot_check.bind_usb([device, bridge], "com25", "test:12"), "COM25")
        for port, serial_number in (("COM26", "TEST:12"), ("COM27", "TEST:12"), ("COM25", "OTHER")):
            with self.subTest(port=port), self.assertRaises(ConsoleError):
                boot_check.bind_usb([device, bridge], port, serial_number)
        duplicate = SimpleNamespace(**{**vars(device), "device": "COM26"})
        with self.assertRaises(ConsoleError):
            boot_check.bind_usb([device, duplicate], "COM25", "TEST:12")

    def test_reset_boundary_drops_only_pre_reset_bytes_and_does_not_swallow_errors(self):
        resume = Event()
        resume.set()
        events = []
        p = SimpleNamespace(sdk_lock=Lock(), rx_lock=Lock(), rx_resume=resume, partial=b"old payload",
                            serial=SimpleNamespace(reset_input_buffer=lambda: events.append("discard")),
                            mark=lambda: events.append("mark") or 3)

        class Reset:
            def __init__(self, serial, uses_usb):
                if serial is not p.serial or not uses_usb or not p.rx_lock.locked() or resume.is_set():
                    raise AssertionError("unsafe reset boundary")

            def reset(self):
                events.append("reset")
                raise OSError("cannot toggle this port")

        with self.assertRaises(OSError):
            boot_check.chip_reset(p, Reset, lambda: events.append("confirm"), clock=lambda: 8.0)
        self.assertTrue(resume.is_set())
        self.assertEqual((events, p.partial), (["confirm", "discard", "mark", "reset"], b""))

    def test_cli_requires_authorization_before_transport(self):
        args = ["xt.py", "boot-check", "COM25", "--usb-serial", "TEST:12", "--commands", "unused.json",
                "--elf-sha256", SHA, "--expect-app", "companion"]
        for denied in ([], ["--allow", "AT+RESTART"], ["--allow", boot_check.ACTION, "--allow", boot_check.ACTION]):
            with (patch("sys.argv", args + denied), patch.object(xt, "serial", SimpleNamespace()),
                  patch.object(xt, "Port", side_effect=AssertionError("opened before permission")),
                  patch.object(xt, "load_vocabulary", side_effect=AssertionError("read before permission")),
                  redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as stderr,
                  self.assertRaises(SystemExit)):
                xt.main()
            self.assertIn("requires exactly --allow USB_CHIP_RESET", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
