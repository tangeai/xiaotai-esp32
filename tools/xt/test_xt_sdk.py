"""Run the real XT entry points with two scripted local ports, never ADB or UART."""
import contextlib
from collections import deque
import io
from pathlib import Path
import re
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
with patch.dict(sys.modules, {"serial": types.ModuleType("serial"),
                              "serial.tools": types.SimpleNamespace(list_ports=None)}):
    import xt
from sdk_selection import load_product, write
from test_device_console_sdk import platform_rows


SHA = {"a": "ac" * 32, "b": "bd" * 32}


class ByteWire:
    """Drive the real background parser in bounded, deterministic read rounds."""

    def __init__(self, backlog=b"", reply=(), never_drains=False):
        self.pending = bytearray(backlog)
        self.reply = deque(reply)
        self.never_drains = never_drains
        self.sent = []
        self.pumping = False
        self.now = 0.0

    @property
    def in_waiting(self):
        return len(self.pending)

    def read(self, size):
        if self.pumping:
            self.port.stop = True
        result = bytes(self.pending[:size])
        if not self.never_drains:
            del self.pending[:size]
        return result

    def write(self, data):
        self.sent.append(data)
        return len(data)

    def flush(self):
        pass

    def open(self):
        assert self.dtr is False and self.rts is False

    def run_reader(self, _seconds):
        self.now += 0.3
        if self.sent and self.reply:
            self.pending.extend(self.reply.popleft())
        self.pumping = True
        try:
            self.port._read()
        finally:
            self.pumping = self.port.stop = False

    @contextlib.contextmanager
    def connect(self):
        # No physical serial port or real sleeps; the same parser normally runs
        # in the background. Delaying it keeps the stale bytes below Port.mark.
        with (patch.object(xt.serial, "Serial", return_value=self, create=True),
              patch.object(xt.serial, "SerialException", OSError, create=True),
              patch.object(xt.threading, "Thread"),
              patch.object(xt.time, "monotonic", side_effect=lambda: self.now),
              patch.object(xt.time, "sleep", side_effect=self.run_reader)):
            self.port = xt.Port("fixture")
            yield self.port


class Fixture:
    def __init__(self, vocabulary):
        self.vocab = vocabulary
        self.ready = {"a": "idle", "b": "idle"}
        self.records = {"a": [], "b": []}
        self.sent = []
        self.closed = []

    def port(self, name, _log=None):
        def send(command, _wait):
            self.sent.append((name, command))
            if command == "AT+VERSION":
                return [f"+VERSION: board={self.vocab.board} revision=3 version=test platform=host chip=esp32s3 at=1",
                        f"+BUILD: elf_sha256={SHA[name]}", "OK"]
            if command == "AT+HELP":
                return [f"+HELP: {entry.label}" for entry, enabled in self.vocab.entries if enabled] + ["OK"]
            if command == "AT+STATE":
                activity = "none" if self.ready[name] == "idle" else "call"
                return [f"+STATE: started=1 app=home activity={activity} id=0 uptime_ms=2000", "OK"]
            if command == "AT+BIND":
                return [f"+BIND: phase=1 device=dev-{name} verification=- left=0", "OK"]
            if command == "AT+LINK":
                value = 20 if self.ready[name] == "active" else 1
                return [f"+LINK-MEDIA: tx={value} rx={value} tx_dropped=0 rx_dropped=0 stale=0", "OK"]
            if command.startswith("AT+LOG FOLLOW "):
                return ["OK"]
            if command == "AT+CALL":
                return [f"+CALL: kind=device state={self.ready[name]} gen=8", "OK"]
            if command == "AT+CALL DEVICE dev-b":
                self.ready.update(a="outgoing", b="incoming")
                self.event("a", "outgoing")
                self.event("b", "incoming")
            elif command == "AT+CALL ACCEPT":
                self.ready.update(a="active", b="active")
                for side in self.ready:
                    self.event(side, "connected", gen=7)  # prior session must not satisfy either peer
                    self.event(side, "connected")
            elif command == "AT+CALL HANGUP":
                self.ready.update(a="idle", b="idle")
                for side in self.ready:
                    self.event(side, "ended")
            else:
                raise AssertionError(command)
            return ["OK"]

        return types.SimpleNamespace(sdk_exchange=send, mark=lambda: len(self.records[name]),
            since=lambda mark: self.records[name][mark:], close=lambda: self.closed.append(name))

    def event(self, name, label, gen=8):
        rows = self.records[name]
        rows.append(f"+LOG: seq={len(rows) + 1} ms=1 level=I src=call label={label} gen={gen} kind=n value=0")


class XtSdkTest(unittest.TestCase):
    def test_stale_driver_terminal_is_not_a_reply(self):
        wire = ByteWire(b"OK\r\n")
        with wire.connect() as port:
            with self.assertRaisesRegex(xt.ConsoleError, "final serial reply"):
                port.sdk_exchange("AT+STATE", 0.7)
        self.assertEqual(wire.sent, [b"AT+STATE\r\n"])

    def test_fresh_reply_allows_split_progress_and_terminal(self):
        wire = ByteWire(b"OK\r\n", (b"+STATE: active=", b"1\r\n", b"O", b"K\r\n+NOTICE: n=1\r\n"))
        with wire.connect() as port:
            self.assertEqual(port.sdk_exchange("AT+STATE", 0.7), ["+STATE: active=1", "OK"])
            self.assertEqual(port.since(0), ["OK", "+STATE: active=1", "OK", "+NOTICE: n=1"])
        self.assertGreater(wire.now, 0.7)
        self.assertEqual(wire.sent, [b"AT+STATE\r\n"])

    def test_new_ready_aborts_batch_but_old_ready_does_not(self):
        ready = b"+READY: at=1 source=fixture locked=0\r\n"
        wire = ByteWire(ready, (b"+STATE: id=0\r\nOK\r\n",))
        with wire.connect() as port:
            self.assertEqual(port.sdk_exchange("AT+STATE", 0.7), ["+STATE: id=0", "OK"])
        for reply in ((ready, b"OK\r\n"), (b"OK\r\n" + ready,)):
            with self.subTest(reply=reply):
                wire = ByteWire(b"", reply)
                with wire.connect() as port:
                    with self.assertRaisesRegex(xt.ConsoleError, "device restarted"):
                        port.sdk_exchange("AT+STATE", 0.7)
                self.assertEqual(wire.sent, [b"AT+STATE\r\n"])

    def test_unbounded_or_incomplete_backlog_prevents_command(self):
        for wire in (ByteWire(b"+NOTICE: n=1\r\n", never_drains=True), ByteWire(b"O")):
            with self.subTest(backlog=wire.pending), wire.connect() as port:
                with self.assertRaisesRegex(xt.ConsoleError, "fresh reply boundary"):
                    port.sdk_exchange("AT+STATE", 0.7)
                self.assertEqual(wire.sent, [])

    def test_sdk_event_mark_excludes_prewrite_probe_result(self):
        stale = b"+VIDEO-PROBE: result=passed frames=3 error=NONE\r\n"
        current = b"+VIDEO-PROBE: result=failed frames=0 error=BUSY\r\n"
        wire = ByteWire(stale, (b"OK\r\n" + current,))
        with wire.connect() as port:
            self.assertEqual(port.sdk_exchange("AT+VIDEO PROBE", 0.7), ["OK"])
            self.assertEqual(port.since(port.sdk_start), ["OK", current.decode().strip()])

    def test_locked_console_is_not_legacy(self):
        for evidence in ("ERROR: INVALID_STATE CLI_DISABLED", "ERROR: INVALID_STATE STARTING", "+READY: at=1 source=test locked=0"):
            with self.subTest(evidence=evidence):
                sends = []

                def send(line, **_):
                    sends.append(line)
                    return [evidence] if line == "AT+VERSION" else ["OK"]

                port = types.SimpleNamespace(send=send, close=lambda: None)
                args = types.SimpleNamespace(dev="fixture", commands=["AT+CLI ON", "AT+RESTART"], wait=2, secs=0, legacy=True)
                with (patch.object(xt, "Port", return_value=port), patch.object(xt, "resolve", return_value=("dev", {"port": "test"})),
                      contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(xt.ConsoleError, "SDK")):
                    xt.cmd_at(args)
                self.assertEqual(sends, ["AT+VERSION"])

    def test_silent_console_allows_only_diagnostics_without_legacy_assurance(self):
        sends = []

        def send(line, **_):
            sends.append(line)
            return [] if line == "AT+VERSION" else ["OK"]

        port = types.SimpleNamespace(send=send, close=lambda: None)
        args = types.SimpleNamespace(dev="fixture", commands=["AT+RESTART"], wait=2, secs=0)
        with (patch.object(xt, "Port", return_value=port), patch.object(xt, "resolve", return_value=("dev", {"port": "test"})),
              contextlib.redirect_stdout(io.StringIO())):
            with self.assertRaisesRegex(xt.ConsoleError, "unidentified console"):
                xt.cmd_at(args)
            self.assertEqual(sends, ["AT+VERSION"])
            args.commands = ["AT+STATUS"]
            xt.cmd_at(args)
            self.assertEqual(sends[-2:], ["AT+VERSION", "AT+STATUS"])
            args.legacy = True
            args.commands = ["ai-start"]
            xt.cmd_at(args)
            self.assertEqual(sends[-2:], ["AT+VERSION", "ai-start"])

        port.send = lambda *_args, **_kwargs: ["+VERSION: board=Xmini-C3-V3 version=1.0 idf=v6.0 mac=12:34:56:78:9a:bc"]
        xt.require_legacy(port)

    def test_canonical_camera_probe_checks_async_result(self):
        calls = []
        session = types.SimpleNamespace(device={"board": "wt99p461-s1"}, check=lambda _: None, send=lambda *_args, **_kwargs: ["OK"])

        def wait(pattern, timeout, mark):
            calls.append((timeout, mark))
            return re.match(pattern, "+VIDEO-PROBE: result=failed frames=0 error=BUSY")

        port = types.SimpleNamespace(sdk_start=9, wait_for=wait, close=lambda: None)
        args = types.SimpleNamespace(dev="fixture", commands=["AT+VIDEO PROBE"], commands_file="known", wait=2, secs=0)
        with (patch.object(xt, "Port", return_value=port), patch.object(xt, "resolve", return_value=("dev", {"port": "test"})),
              patch.object(xt, "sdk_session", return_value=session), contextlib.redirect_stdout(io.StringIO()),
              self.assertRaisesRegex(xt.ConsoleError, "video probe failed")):
            xt.cmd_at(args)
        self.assertEqual(calls, [(15, 9)])

    def run_pair(self, follow=False, peer_sha=SHA["b"]):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            product = load_product(ROOT / "products/xiaotai-cores3-lite/product.json")
            write(ROOT, product, directory)
            commands = directory / "commands.json"
            vocabulary = xt.load_vocabulary(commands)
            fixture = Fixture(vocabulary)
            allows = ["AT+CALL DEVICE dev-b", "AT+CALL ACCEPT", "AT+CALL HANGUP"]
            if follow:
                allows += ["AT+LOG FOLLOW 1", "AT+LOG FOLLOW 0"]
            args = types.SimpleNamespace(commands_file=commands, peer_commands=commands, elf_sha256=SHA["a"],
                peer_elf_sha256=peer_sha, allow=allows, secs=0, ring=0)
            with (patch.object(xt, "Port", fixture.port), patch.object(xt, "run_dir", return_value=directory),
                  contextlib.redirect_stdout(io.StringIO()), patch.object(xt, "finish") as done):
                xt.sdk_pair(args, "a", {"port": "a"}, "b", {"port": "b"})
            return fixture, done.call_args.args[1]

    def test_follow_and_fallback_poll(self):
        for follow in (True, False):
            with self.subTest(follow=follow):
                fixture, summary = self.run_pair(follow=follow)
                self.assertTrue(summary["pass"], summary)
                self.assertEqual(summary["caller_media"]["rx"], 19)
                self.assertEqual(fixture.closed, ["b", "a"])
                if follow:
                    self.assertEqual([row for row in fixture.sent if row[1].startswith("AT+LOG FOLLOW")],
                        [("a", "AT+LOG FOLLOW 1"), ("b", "AT+LOG FOLLOW 1"),
                         ("b", "AT+LOG FOLLOW 0"), ("a", "AT+LOG FOLLOW 0")])
                else:
                    self.assertIn(("b", "AT+CALL"), fixture.sent)

    def test_mismatched_peer_does_not_dial(self):
        fixture, summary = self.run_pair(peer_sha=SHA["a"])
        self.assertFalse(summary["pass"])
        self.assertIn("fingerprint", summary["failure"])
        self.assertNotIn(("a", "AT+CALL DEVICE dev-b"), fixture.sent)

    def run_wechat_in(self, fail_accept=None, allow_reject=True, confirm=True, *,
                      expected="dev-a", phase=1, signed=False, online=True, authorized=True,
                      signaling=True, phone_error=None,
                      phone_hangup=False, rebind=False):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            write(ROOT, load_product(ROOT / "products/xiaotai-cores3-lite/product.json"), directory)
            commands = directory / "commands.json"
            fixture = Fixture(xt.load_vocabulary(commands))
            port = fixture.port("a")
            ordinary, generation = 0, 0
            bound = "dev-a"
            fallthrough, wait_call = port.sdk_exchange, xt.sdk_bench.wait_call

            def exchange(command, wait):
                nonlocal ordinary, generation
                if command == "AT+BIND":
                    return [f"+BIND: phase={phase} signed={int(signed)} device={bound}", "OK"]
                if command == "AT+PLATFORM":
                    return [f"+PLATFORM: online={int(online)} authorized={int(authorized)} signaling={int(signaling)}",
                            *platform_rows()[1:], "OK"]
                if command == "AT+CALL":
                    fixture.sent.append(("a", command))
                    state = "incoming" if ordinary else "idle"
                    return [f"+CALL: kind=device state={state} gen={ordinary}",
                            f"+CALL: kind=wechat state={fixture.ready['a']} gen={generation}", "OK"]
                if command in {"AT+CALL ACCEPT", "AT+CALL REJECT", "AT+CALL HANGUP"}:
                    fixture.sent.append(("a", command))
                    if command == "AT+CALL ACCEPT":
                        if fail_accept:
                            fixture.ready["a"], generation, ordinary = fail_accept
                            return ["ERROR: BUSY"]
                        fixture.ready["a"] = "active"
                    else:
                        # Strict firmware keeps a ringing call until explicit REJECT.
                        if (command == "AT+CALL REJECT") != (fixture.ready["a"] == "incoming"):
                            return ["ERROR: INVALID_STATE"]
                        fixture.ready["a"], generation = "idle", 0
                    return ["OK"]
                return fallthrough(command, wait)

            def ring(recheck, *, read_ui):
                nonlocal generation, bound
                self.assertIs(read_ui, xt.read_phone_ui)
                if phone_error:
                    raise xt.ConsoleError(phone_error)
                if rebind:
                    bound = "dev-b"
                recheck()
                calls.append("tap")
                fixture.ready["a"], generation = "incoming", 8

            def observe(*args, **kwargs):
                if not confirm:
                    raise xt.ConsoleError("unconfirmed incoming event")
                return wait_call(*args, **kwargs)

            port.sdk_exchange = exchange
            allows = ["AT+CALL ACCEPT", "AT+CALL HANGUP"] + (["AT+CALL REJECT"] if allow_reject else [])
            args = types.SimpleNamespace(commands_file=commands, elf_sha256=SHA["a"], allow=allows,
                                         secs=0, ring=0, phone_hangup=phone_hangup)
            calls = []
            phone = types.SimpleNamespace(dial=ring)
            with (patch.object(xt, "Port", return_value=port), patch.object(xt, "run_dir", return_value=directory),
                  patch.object(xt, "phone_profile", return_value={}),
                  patch.object(xt.wx_phone, "VerifiedIncomingPhone", return_value=phone) as prepared,
                  patch.object(xt, "reopen_miniprogram", side_effect=AssertionError("unsafe phone navigation")),
                  patch.object(xt, "tap", side_effect=AssertionError("unsafe fixed coordinate")),
                  patch.object(xt, "hangup_on_phone", side_effect=AssertionError("unsafe phone hangup")),
                  patch.object(xt.sdk_bench, "wait_call", side_effect=observe),
                  patch.object(xt, "finish") as done):
                xt.sdk_wechat_run(args, "a", {"port": "a", "device_id": expected}, incoming=True)
            self.assertEqual(fixture.closed, ["a"])
            sent = [line for _, line in fixture.sent if line in allows or line == "AT+CALL REJECT"]
            self.assertLessEqual(len(calls), 1)
            return done.call_args.args[1], sent, prepared.called, bool(calls)

    def test_wechat_in_needs_reject_authority_before_phone_actions(self):
        summary, sent, reopened, tapped = self.run_wechat_in(allow_reject=False)
        self.assertFalse(summary["pass"])
        self.assertIn("AT+CALL REJECT", summary["failure"])
        self.assertEqual((sent, reopened, tapped), ([], False, False))

    def test_wechat_in_blocks_wrong_or_offline_board_before_phone(self):
        for options, message in (({"expected": "dev-b"}, "reviewed binding"),
                                 ({"expected": None}, "exact board binding ID"),
                                 ({"expected": "-"}, "exact board binding ID"),
                                 ({"phase": 6, "signed": True}, "reviewed binding"),
                                 ({"online": False}, "signaling is not online"),
                                 ({"authorized": False, "signed": True}, "platform is not authorized"),
                                 ({"signaling": False}, "signaling is not online"),
                                 ({"phone_hangup": True}, "does not verify phone-side hangup")):
            with self.subTest(options=options):
                summary, sent, prepared, tapped = self.run_wechat_in(**options)
                self.assertFalse(summary["pass"])
                self.assertIn(message, summary["failure"])
                self.assertEqual((sent, prepared, tapped), ([], False, False))

    def test_wechat_in_no_call_action_on_unverified_phone_or_binding_drift(self):
        for options in ({"phone_error": "reviewed device row not visible"}, {"rebind": True}):
            with self.subTest(options=options):
                summary, sent, prepared, tapped = self.run_wechat_in(**options)
                self.assertFalse(summary["pass"])
                self.assertIn("reviewed", summary["failure"])
                self.assertEqual((sent, prepared, tapped), ([], True, False))
                self.assertNotIn("phone_call_tapped", summary)

    def test_wechat_in_failure_cleans_the_confirmed_phase(self):
        for state, cleanup in (("incoming", "REJECT"), ("connecting", "HANGUP"), ("active", "HANGUP")):
            with self.subTest(state=state):
                summary, sent, _, _ = self.run_wechat_in(fail_accept=(state, 8, 0))
                self.assertFalse(summary["pass"])
                self.assertIn("BUSY", summary["failure"])
                self.assertNotIn("cleanup_errors", summary)
                self.assertEqual(sent, ["AT+CALL ACCEPT", f"AT+CALL {cleanup}"])

    def test_wechat_in_never_cleans_unconfirmed_or_other_calls(self):
        for failure, confirm in ((("incoming", 9, 0), True), (("incoming", 8, 42), True), (None, False)):
            with self.subTest(failure=failure, confirm=confirm):
                summary, sent, _, tapped = self.run_wechat_in(fail_accept=failure, confirm=confirm)
                self.assertFalse(summary["pass"])
                self.assertTrue(tapped)
                self.assertTrue(summary["cleanup_errors"])
                self.assertEqual(sent, ["AT+CALL ACCEPT"] if confirm else [])

    def test_wechat_in_normal_session_still_hangs_up(self):
        summary, sent, _, tapped = self.run_wechat_in()
        self.assertTrue(summary["pass"], summary)
        self.assertTrue(tapped and summary["phone_call_tapped"] and summary["phone_target_verified"])
        self.assertEqual(sent, ["AT+CALL ACCEPT", "AT+CALL HANGUP"])

    def test_wechat_in_already_ended_needs_no_cleanup_action(self):
        summary, sent, _, _ = self.run_wechat_in(fail_accept=("ended", 0, 0))
        self.assertFalse(summary["pass"])
        self.assertNotIn("cleanup_errors", summary)
        self.assertEqual(sent, ["AT+CALL ACCEPT"])

    def run_wechat_out(self, *, answer=True, receipt=True, generation=8, cleanup_generation=None,
                       allow_dial=True, phone_hangup=False, expected="dev-a",
                       gate_confirm=None, drift_after_pickup=False, newer_call=None):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            write(ROOT, load_product(ROOT / "products/xiaotai-cores3-lite/product.json"), directory)
            commands = directory / "commands.json"
            fixture = Fixture(xt.load_vocabulary(commands))
            port = fixture.port("a")
            fallback = port.sdk_exchange
            command = "AT+CALL INDEX 5"
            hangup = "AT+CALL HANGUP"
            gen = generation
            drift = False
            replacement_seen = False

            def confirm(action):
                nonlocal gen
                if action == hangup and newer_call == "during-hangup-confirm" and not replacement_seen:
                    gen = generation + 1
                return gate_confirm(action)

            def exchange(text, wait):
                nonlocal gen, replacement_seen
                if text == "AT+CONTACTS":
                    return ["+CONTACT: source=wechat index=5", "OK"]
                if text == "AT+VERSION" and drift:
                    return [line.replace(SHA["a"], SHA["b"]) for line in fallback(text, wait)]
                if text == "AT+LINK" and newer_call == "before-hangup" and fixture.ready["a"] == "active":
                    gen = generation + 1
                if text not in {"AT+CALL", command, hangup}:
                    return fallback(text, wait)
                fixture.sent.append(("a", text))
                if text == command:
                    fixture.ready["a"] = "outgoing"
                elif text == hangup:
                    fixture.ready["a"] = "idle"
                    gen = 0
                row = f"+CALL: kind=wechat state={fixture.ready['a']} gen={gen}"
                if text == "AT+CALL" and newer_call == "during-hangup-confirm" and gen == generation + 1:
                    replacement_seen = True
                    gen = generation  # A later stale read must not turn the previous denial into permission.
                return [row, "OK"]

            def pickup(*, confirm=None):
                nonlocal gen, drift
                if confirm is not None:
                    if newer_call == "before-answer":
                        gen = generation + 1
                    confirm("PHONE_WX_OUT_ANSWER")
                if answer:
                    fixture.ready["a"] = "active"
                    drift = drift_after_pickup
                elif cleanup_generation is not None:
                    gen = cleanup_generation
                return answer

            def ended():
                if not receipt:
                    raise xt.ConsoleError("phone receipt missing")
                return 10

            port.sdk_exchange = exchange
            phone = types.SimpleNamespace(answer=pickup)
            args = types.SimpleNamespace(commands_file=commands, elf_sha256=SHA["a"],
                                         allow=[command, hangup] if allow_dial else [hangup],
                                         secs=0, ring=0, phone_hangup=phone_hangup)
            if gate_confirm is not None:
                args.gate_confirm = confirm
                args.gate_phone_serial = "selected"
            with (patch.object(xt, "Port", return_value=port), patch.object(xt, "run_dir", return_value=directory),
                  patch.object(xt, "outgoing_phone", return_value=phone) as selected,
                  patch.object(xt, "outgoing_phone_receipt", side_effect=lambda _: ended()),
                  patch.object(xt, "outgoing_phone_hangup", side_effect=xt.ConsoleError("unknown phone page")),
                  patch.object(phone, "answer", wraps=pickup) as picked, patch.object(xt, "finish") as done):
                output = xt.sdk_wechat_run(args, "a", {"port": "a", "device_id": expected}, incoming=False)
            actions = [text for _, text in fixture.sent if text in {command, hangup}]
            self.assertEqual(output, (directory, done.call_args.args[1]))
            if gate_confirm is not None and selected.called:
                self.assertEqual(selected.call_args.kwargs, {"expected_serial": "selected"})
            return done.call_args.args[1], actions, selected.called, picked.called

    def test_wechat_out_requires_grants_and_selected_runtime_identity_before_phone(self):
        for kwargs in ({"allow_dial": False}, {"expected": "other"}):
            summary, actions, selected, picked = self.run_wechat_out(**kwargs)
            self.assertFalse(summary["pass"])
            self.assertEqual((actions, selected, picked), ([], False, False))

    def test_wechat_out_no_unconfirmed_answer_or_other_generation_cleanup(self):
        summary, actions, _, picked = self.run_wechat_out(generation=0)
        self.assertFalse(summary["pass"])
        self.assertFalse(picked)
        self.assertEqual(actions, ["AT+CALL INDEX 5"])
        summary, actions, _, picked = self.run_wechat_out(answer=False, cleanup_generation=9)
        self.assertFalse(summary["pass"])
        self.assertTrue(picked)
        self.assertEqual(actions, ["AT+CALL INDEX 5"])

    def test_wechat_out_unmatched_banner_ends_only_the_current_device_call(self):
        summary, actions, _, picked = self.run_wechat_out(answer=False)
        self.assertFalse(summary["pass"])
        self.assertTrue(picked)
        self.assertNotIn("phone_answer_tapped", summary)
        self.assertEqual(actions, ["AT+CALL INDEX 5", "AT+CALL HANGUP"])

    def test_wechat_out_phone_evidence_is_distinct_from_device_connected(self):
        for receipt in (True, False):
            summary, actions, _, _ = self.run_wechat_out(receipt=receipt)
            self.assertEqual(summary["pass"], receipt, summary)
            self.assertTrue(summary["device_connected"])
            self.assertTrue(summary["phone_answer_tapped"])
            self.assertEqual(summary.get("phone_connected_duration_s"), 10 if receipt else None)
            self.assertEqual(actions, ["AT+CALL INDEX 5", "AT+CALL HANGUP"])

    def test_wechat_out_unknown_phone_hangup_uses_same_device_generation_for_cleanup(self):
        summary, actions, _, _ = self.run_wechat_out(phone_hangup=True)
        self.assertFalse(summary["pass"])
        self.assertIn("unknown phone page", summary["failure"])
        self.assertEqual(actions, ["AT+CALL INDEX 5", "AT+CALL HANGUP"])

    def test_gate_wx_out_scope_renewal_and_denied_answer_cleanup(self):
        checked = []

        def authorize(action):
            checked.append(action)
            return action != "PHONE_WX_OUT_ANSWER"

        summary, actions, _, _ = self.run_wechat_out(gate_confirm=authorize)
        self.assertFalse(summary["pass"])
        self.assertTrue(summary["gate_started"])
        self.assertEqual(checked, ["AT+CALL INDEX 5", "PHONE_WX_OUT_ANSWER", "AT+CALL HANGUP"])
        self.assertEqual(actions, ["AT+CALL INDEX 5", "AT+CALL HANGUP"])
        self.assertNotIn("phone_answer_tapped", summary)

    def test_gate_wx_out_refuses_runtime_fingerprint_change_and_unconfirmed_dial(self):
        summary, actions, _, picked = self.run_wechat_out(gate_confirm=lambda _: False)
        self.assertFalse(summary["pass"])
        self.assertFalse(picked)
        self.assertNotIn("gate_started", summary)
        self.assertEqual(actions, [])
        summary, actions, _, _ = self.run_wechat_out(gate_confirm=lambda _: True, drift_after_pickup=True)
        self.assertFalse(summary["pass"])
        self.assertTrue(summary["gate_started"])
        self.assertIn("running device identity changed", summary["failure"])
        self.assertTrue(summary["cleanup_errors"])
        self.assertEqual(actions, ["AT+CALL INDEX 5"])

    def test_gate_wx_out_does_not_answer_or_hang_up_a_replaced_call(self):
        for at in ("before-answer", "before-hangup"):
            with self.subTest(at=at):
                summary, actions, _, _ = self.run_wechat_out(gate_confirm=lambda _: True, newer_call=at)
                self.assertFalse(summary["pass"])
                self.assertEqual(actions, ["AT+CALL INDEX 5"])
                self.assertTrue(summary.get("cleanup_errors"))
                self.assertEqual(summary.get("phone_answer_tapped", False), at == "before-hangup")

    def test_standalone_wx_out_does_not_answer_or_hang_up_a_replaced_call(self):
        for at in ("before-answer", "before-hangup"):
            with self.subTest(at=at):
                summary, actions, _, _ = self.run_wechat_out(newer_call=at)
                self.assertFalse(summary["pass"])
                self.assertEqual(actions, ["AT+CALL INDEX 5"])
                self.assertTrue(summary.get("cleanup_errors"))
                self.assertEqual(summary.get("phone_answer_tapped", False), at == "before-hangup")

    def test_gate_wx_out_checks_owner_after_confirm_and_does_not_replay_denial(self):
        for answer in (True, False):
            with self.subTest(normal_hangup=answer):
                checked = []

                def allow(action):
                    checked.append(action)
                    return True

                summary, actions, _, _ = self.run_wechat_out(answer=answer, gate_confirm=allow,
                                                             newer_call="during-hangup-confirm")
                self.assertFalse(summary["pass"])
                self.assertEqual(actions, ["AT+CALL INDEX 5"])
                self.assertEqual(checked.count("AT+CALL HANGUP"), 1)
                self.assertIn("generation changed", summary["cleanup_errors"][0])

    def test_trace_verifies_direction_indexes_and_duration(self):
        session = types.SimpleNamespace(send=lambda *_args, **_kwargs: [
            "+TRACE: active=1 tx=1 rx=1 capacity=2 skipped=0", "+TRACE:0,0,3,5,55abc123,7,8,9",
            "+TRACE:1,0,5,5,55abc123,7,8,9,18000,40", "OK"])
        parsed = xt.dump_trace(session, sdk=True)
        self.assertEqual(parsed[1][0]["duration_ms"], 40)
        self.assertEqual(parsed[1][0]["sender_ms"], 18000)
