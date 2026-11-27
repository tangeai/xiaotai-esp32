from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import xt
import gate
from console_sdk import ConsoleError
from sdk_selection import load_product, write
from test_boot_check import Bench, C3_USB_BANNER, ONLINE, SHA, STATS, vocabulary


PRODUCT = "xiaotai-szpi-s3"
BOARD = "lckfb-szpi-esp32s3"
USB = "test:driver"


class Port:
    def __init__(self, bus, name, *, exclusive):
        assert name == "COM25" and exclusive
        self.bus = bus

    def sdk_exchange(self, command, wait):
        self.bus.append(command)
        values = {
            "AT+VERSION": [f"+VERSION: board={BOARD} chip=esp32s3 at=1", f"+BUILD: elf_sha256={SHA}"],
            "AT+HELP": ["+HELP: " + name for name in ("AT", "AT+VERSION", "AT+STATE", "AT+NET", "AT+PLATFORM",
                                                       "AT+STATUS [<section>]", "AT+MEM")],
            "AT+STATE": ["+STATE: started=1 app=companion activity=none id=0 uptime_ms=30000"],
            "AT+NET": ["+NET: state=up connected=1 provisioning=0 held=0 ip=192.0.2.5 ssid_hex=74657374"],
            "AT+PLATFORM": [ONLINE, *STATS],
            "AT+STATUS tasks": ["+TASKS: supported=1 count=1", "+TASK: name=xiaotai stack_min_free=1200 priority=4 core=0"],
            "AT+MEM": ["+MEM: kind=internal free=10000 min_free=8000 largest=6000"],
        }
        if command not in values:
            raise AssertionError(f"read-only gate attempted {command}")
        return [*values[command], "OK"]

    def mark(self):
        return 0

    def close(self):
        pass


class GateTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="xt gate ")
        self.addCleanup(directory.cleanup)
        self.out = Path(directory.name)
        write(xt.ROOT, load_product(xt.ROOT / "products" / PRODUCT / "product.json"), self.out)
        self.manifest = self.out / "permissions.json"
        self.data = {"schema": 1, "primary": {"usb_serial": USB, "elf_sha256": SHA,
                                              "commands": "commands.json", "actions": []}, "stack_tasks": ["xiaotai"]}
        self.args = SimpleNamespace(dev="COM25", peer=None, phone=False, product=PRODUCT, elf_sha256=SHA)

    def prepare(self):
        self.manifest.write_text(json.dumps(self.data), encoding="utf-8")
        return gate.load(self.manifest, xt.ROOT, self.args, lambda dev: (dev, {"port": dev}))

    def kit(self, descriptors):
        traffic = []
        kit = SimpleNamespace(Port=lambda *args, **kwargs: Port(traffic, *args, **kwargs), BOOT_BANNER=xt.BOOT_BANNER,
                              list_ports=SimpleNamespace(comports=lambda: descriptors))
        return kit, traffic

    def test_no_action_gate_covers_ten_rows_and_writes_evidence(self):
        kit, traffic = self.kit([SimpleNamespace(device="COM25", serial_number=USB)])
        report = gate.Gate(kit, *self.prepare(), self.out).run()
        statuses = {row["item"]: row["status"] for row in report["items"]}
        self.assertEqual(len(statuses), 10)
        self.assertEqual({name for name in statuses if statuses[name] == "pass"}, {"network", "serial"})
        self.assertEqual(report["verdict"], "not_verified")
        self.assertTrue(report["complete"])
        partial = gate.Gate(kit, *self.prepare(), self.out).summary([{"status": "pass"}])
        self.assertEqual((partial["complete"], partial["verdict"]), (False, "not_verified"))
        self.assertIn("AT+STATUS tasks", traffic)
        self.assertIn("AT+MEM", traffic)
        self.assertEqual(json.loads((self.out / "summary.json").read_text(encoding="utf-8")), report)
        md = (self.out / f"基础功能关-{PRODUCT}.md").read_text(encoding="utf-8")
        self.assertIn("[network address and application platform online](network.json)", md)
        self.assertNotIn(USB, md)
        self.assertNotIn("192.0.2.5", repr(report))

    def test_disappearing_or_ambiguous_port_never_opens(self):
        for rows in ([], [SimpleNamespace(device="COM26", serial_number=USB)],
                     [SimpleNamespace(device=f"COM{number}", serial_number=USB) for number in (25, 26)]):
            kit, traffic = self.kit(rows)
            report = gate.Gate(kit, *self.prepare(), self.out).run()
            self.assertFalse(traffic)
            self.assertTrue(all(row["status"] == "not_verified" for row in report["items"]))

    def test_manifest_does_not_mix_elf_peer_or_phone(self):
        self.data["primary"]["elf_sha256"] = "fd" * 32
        with self.assertRaisesRegex(ConsoleError, "ELF differs"):
            self.prepare()
        self.data["primary"]["elf_sha256"] = SHA
        self.data["peer"] = {**self.data["primary"], "product": PRODUCT}
        with self.assertRaisesRegex(ConsoleError, "requires explicit --peer"):
            self.prepare()
        self.args.peer = "COM26"
        with self.assertRaisesRegex(ConsoleError, "different USB"):
            self.prepare()
        self.data.pop("peer")
        self.args.peer = None
        self.data["phone"] = {"serial": "test-phone", "actions": ["PHONE_WX_OUT_ANSWER"]}
        with self.assertRaisesRegex(ConsoleError, "requires explicit --phone"):
            self.prepare()
        self.args.phone = True
        self.assertEqual(self.prepare()[2].permits, {"PHONE_WX_OUT_ANSWER"})

    def test_missing_cleanup_permission_does_not_send_start(self):
        session = Mock(permits={"AT+AI START"})
        with self.assertRaises(gate.Unverified):
            gate.require_permits(session, ("AT+AI START", "AT+AI STOP"))
        session.send.assert_not_called()

    def test_only_explicit_isolated_bench_enables_active_checks(self):
        self.data["primary"]["actions"] = ["USB_CHIP_RESET", "AT+AI START", "AT+AI STOP"]
        kit, traffic = self.kit([SimpleNamespace(device="COM25", serial_number=USB)])
        for bad in ("true", 1, None):
            self.data["isolated_bench"] = bad
            with self.assertRaisesRegex(ConsoleError, "must be a JSON boolean"):
                self.prepare()
        for value in (False, True):
            with self.subTest(isolated=value):
                self.data["isolated_bench"] = value
                runner = gate.Gate(kit, *self.prepare(), self.out)
                with patch.object(runner, "boot", return_value=gate.result("pass", "fake reset")) as boot:
                    report = runner.run()
                self.assertIs(report["isolated_bench"], value)
                self.assertEqual(boot.call_count, int(value))
                if not value:
                    rows = [row for row in report["items"] if row["item"] in gate.ACTIVE_ITEMS]
                    self.assertTrue(all(row["status"] == "not_verified" and "isolated_bench" in row["detail"]
                                        for row in rows))
                    self.assertNotIn("AT+AI START", traffic)

    def test_native_boot_gate_dispatches_three_real_rom_checks_on_s3_and_c3(self):
        run_boot_check = gate.boot_check.run
        for chip, port in (("esp32s3", "COM25"), ("esp32c3", "COM9")):
            with self.subTest(chip=chip):
                bench = Bench()
                bench.chip = chip
                if chip == "esp32c3":
                    bench.before = list(C3_USB_BANNER)
                    bench.new_lines = C3_USB_BANNER
                native = SimpleNamespace(device=port, serial_number=USB, vid=0x303A, pid=0x1001)
                target = SimpleNamespace(chip=chip, port=port, usb=USB, permits={gate.boot_check.ACTION},
                                         vocab=vocabulary(), sha=SHA, app="companion")

                def open_port(selected, *, exclusive):
                    self.assertEqual(selected, port)
                    self.assertTrue(exclusive)
                    bench.opens += 1
                    bench.replies = bench.before.copy()
                    return SimpleNamespace(sdk_exchange=bench.sdk_exchange, since=bench.since,
                                           close=Mock(side_effect=lambda: setattr(bench, "closes", bench.closes + 1)))

                def fake_reset(endpoint, reset, confirm):
                    self.assertEqual(endpoint.sdk_exchange, bench.sdk_exchange)
                    self.assertEqual(confirm(), port)
                    return bench.reset(bench)

                kit = SimpleNamespace(Port=open_port, list_ports=SimpleNamespace(comports=lambda: [native]))
                runner = gate.Gate(kit, target, None, None, [], True, self.out)
                fake_esptool = SimpleNamespace(reset=SimpleNamespace(HardReset=SimpleNamespace(reset=lambda: None)))
                with (patch.dict(sys.modules, {"esptool": fake_esptool, "esptool.reset": fake_esptool.reset}),
                      patch.object(gate.boot_check, "chip_reset", side_effect=fake_reset),
                      patch.object(gate.boot_check, "run", side_effect=lambda *args: run_boot_check(
                          *args, clock=bench.clock, sleep=bench.sleep))):
                    result = runner.boot()
                self.assertEqual(result["status"], "pass", result)
                self.assertEqual((bench.opens, bench.closes, bench.reset_attempts), (3, 3, 3))
                self.assertTrue(all(row["rom_reset"] and row["boot"] for row in result["facts"]["rounds"]))

    def test_gate_does_not_dispatch_native_reset_for_p4_or_without_permission(self):
        for chip, permits in (("esp32p4", {gate.boot_check.ACTION}), ("esp32c3", set())):
            with self.subTest(chip=chip):
                kit = Mock()
                target = SimpleNamespace(chip=chip, permits=permits)
                result = gate.Gate(kit, target, None, None, [], True, self.out).boot()
                self.assertEqual(result["status"], "not_verified")
                kit.list_ports.comports.assert_not_called()
                kit.Port.assert_not_called()

    def test_worker_watermarks_are_evidence_even_without_the_full_task_table(self):
        for transport, expected in ((1023, "fail"), (1024, "not_verified"), (0, "not_verified")):
            with self.subTest(transport=transport):
                stats = [row.replace("worker_stack_free=0", "worker_stack_free=1050") for row in STATS]
                replies = {"AT+STATUS tasks": ["+TASKS: supported=0 count=0", "OK"],
                           "AT+STATUS port": ["+PORT: cache=0", "OK"],
                           "AT+LINK": [f"+LINK: worker_stack_free={transport}", "OK"],
                           "AT+PLATFORM": [ONLINE, *stats, "OK"]}
                session = SimpleNamespace(check=lambda command: SimpleNamespace(access="read"),
                                          send=replies.__getitem__, has=replies.__contains__)
                runner = gate.Gate(None, None, None, None, ["main"], False, self.out)
                row = runner.memory_stack(SimpleNamespace(session=session))
                self.assertEqual(row["status"], expected)
                self.assertEqual(row["facts"]["stack"]["all_application_tasks"], "not_measured")
                workers = row["facts"]["worker_watermarks"]
                self.assertEqual(workers["transport"]["stack_min_free_bytes"], transport or None)
                self.assertEqual(workers["platform"]["stack_min_free_bytes"], 1050)

    def test_gate_cli_does_not_route_to_legacy_regress(self):
        binding = self.prepare()
        argv = ["xt.py", "gate", "COM25", "--product", PRODUCT, "--elf-sha256", SHA,
                "--allow-file", str(self.manifest)]
        with (patch("sys.argv", argv), patch.object(xt, "serial", SimpleNamespace()),
              patch.object(gate, "load", return_value=binding), patch.object(xt, "run_dir", return_value=self.out),
              patch.object(xt, "list_ports", SimpleNamespace(comports=lambda: [])),
              patch.object(xt, "cmd_regress", side_effect=AssertionError("gate must not call legacy")),
              patch.object(xt, "Port", side_effect=AssertionError("no selected port")),
              patch.object(xt.subprocess, "run", side_effect=AssertionError("no subprocess")),
              redirect_stdout(io.StringIO()) as out, self.assertRaises(SystemExit) as stopped):
            xt.main()
        self.assertEqual(stopped.exception.code, 2)
        self.assertIn("gate: not_verified", out.getvalue())

    def test_gate_outbound_adapter_preserves_only_explicit_grants(self):
        self.data["primary"]["actions"] = ["USB_CHIP_RESET", "AT+CALL INDEX 5", "AT+CALL HANGUP"]
        target = self.prepare()[0]
        callback = lambda action: action == "PHONE_WX_OUT_ANSWER"
        expected = (self.out, {"pass": True})
        with patch.object(xt, "sdk_wechat_run", return_value=expected) as runner:
            self.assertEqual(xt.gate_wx_out(target, "confirmed-phone", callback), expected)
        args, name, info = runner.call_args.args
        self.assertEqual(set(args.allow), {"AT+CALL INDEX 5", "AT+CALL HANGUP"})
        self.assertEqual((args.elf_sha256, args.gate_phone_serial), (SHA, "confirmed-phone"))
        self.assertEqual((args.phone_hangup, name, info["port"], runner.call_args.kwargs["incoming"]),
                         (False, "COM25", "COM25", False))


if __name__ == "__main__":
    unittest.main()
