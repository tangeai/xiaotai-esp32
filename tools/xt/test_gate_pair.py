import json
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import xt  # noqa: F401 (puts scripts/ on the import path)
import gate_pair
from console_sdk import ConsoleError


READS = {"AT+BIND", "AT+CALL", "AT+STATUS call", "AT+LINK", "AT+STATUS media", "AT+STATE"}


class Pair:
    def __init__(self):
        self.ids = ["fixture-left-private", "fixture-right-private"]
        self.rounds = 0
        self.phase = ["idle", "idle"]
        self.gens = [0, 0]
        self.peer = ["-", "-"]
        self.grants = [{gate_pair.ACCEPT, gate_pair.REJECT, gate_pair.HANGUP, f"AT+CALL DEVICE {self.ids[1 - i]}"}
                       for i in range(2)]
        self.sent = []
        self.timeline = []
        self.missing_audio = False
        self.slot_state = 2
        self.foreign_incoming = False
        self.foreign_wechat = False
        self.reuse_gen = False
        self.fail_accept = False
        self.keep_callee_ringing = False
        self.reappear = False
        self.reappear_during_audio = False
        self.idle_seen = False
        self.confirmable = [True, True]
        self.bind_reads = [0, 0]
        self.lose_usb_after_last_dial_probe = False
        self.audio_count = [0, 0]
        self.written = [0, 0]
        self.no_output = None
        self.no_output_counter = False
        self.foreign_media = False
        self.reset_output_counter = False

    def endpoint(self, side):
        def check(command):
            if command not in READS and command not in self.grants[side]:
                raise ConsoleError(f"not allowed {command}")

        def checkpoint(idle=False):
            self.timeline.append((side, "checkpoint"))
            if idle and self.phase[side] != "idle":
                raise ConsoleError("fixture is not idle")

        def confirm():
            if not self.confirmable[side]:
                raise ConsoleError("fixture USB device changed")
            self.timeline.append((side, "confirm"))
            return f"fixture port {side}"

        def send(command):
            check(command)
            if command not in READS and self.timeline[-1:] != [(side, "confirm")]:
                raise AssertionError("action lacked a fresh physical endpoint confirmation")
            self.timeline.append((side, command))
            return self.command(side, command)

        return SimpleNamespace(port=object(), session=SimpleNamespace(check=check, send=send),
                               witness=SimpleNamespace(checkpoint=checkpoint), confirm=confirm)

    def call_row(self, side):
        rows = []
        if self.foreign_wechat and side == 1 and self.phase[side] == "incoming":
            rows.append("+CALL: kind=wechat state=active gen=99 peer=-")
        return rows + [f"+CALL: kind=device state={self.phase[side]} gen={self.gens[side]} peer={self.peer[side]}"]

    def command(self, i, command):
        if command == "AT+BIND":
            self.bind_reads[i] += 1
            if self.lose_usb_after_last_dial_probe and i == 1 and self.bind_reads[i] == 2:
                self.confirmable[0] = False
            return [f"+BIND: device={self.ids[i]}", "OK"]
        if command == "AT+CALL":
            if i == 0 and self.idle_seen and self.reappear and not self.gens[0]:
                self.phase[0], self.gens[0], self.peer[0] = "active", 11, self.ids[1]
            return self.call_row(i) + ["OK"]
        if command == "AT+STATE":
            if self.reappear and i == 0 and self.sent and self.sent[-1] == (0, gate_pair.HANGUP):
                self.idle_seen = True
            activity = "none id=0" if self.phase[i] == "idle" else "call id=4"
            return [f"+STATE: started=1 activity={activity}", "OK"]
        if command == "AT+STATUS call":
            if i == 0 and self.reappear_during_audio:
                # A same-boot idle snapshot is observed, but the prior gen
                # appears again when the failure handler next reads AT+CALL.
                return ["+CALL: kind=device state=ended gen=0 peer=-", "+CALL-DIAG: link=0", "OK"]
            return self.call_row(i) + [f"+CALL-DIAG: link={self.gens[i] + 100}", "OK"]
        if command == "AT+LINK":
            if not self.missing_audio:
                self.audio_count[i] += 20
            value = self.audio_count[i]
            slot_state = "" if self.slot_state is None else f" state={self.slot_state}"
            return ["+LINK-SLOT: gen=999 state=2 audio_tx=876 audio_rx=543",
                    f"+LINK-SLOT: gen={self.gens[i] + 100}{slot_state} audio_tx={value} audio_rx={value}", "OK"]
        if command == "AT+STATUS media":
            active = self.phase[i] == "active"
            gen = self.gens[i] + (101 if self.foreign_media else 100) if active else 0
            if active and self.no_output != i:
                self.written[i] += 320
            value = self.audio_count[i]
            written = self.written[i] if not self.reset_output_counter or self.written[i] < 640 else 0
            output = "" if self.no_output_counter else f" pcm_written={written} write_failed=0"
            return [f"+MEDIA: active={int(active)} generation={gen}",
                    f"+MEDIA-RX: received={value} played={value}{output}", "OK"]
        self.sent.append((i, command))
        if command == f"AT+CALL DEVICE {self.ids[1 - i]}":
            self.rounds += 1
            generation = 1 if self.reuse_gen else self.rounds
            self.gens = [10 + generation, 20 + generation]
            self.phase[i], self.phase[1 - i] = "outgoing", "incoming"
            self.peer[i] = self.ids[1 - i]
            self.peer[1 - i] = "different-fixture" if self.foreign_incoming else self.ids[i]
        elif command == gate_pair.ACCEPT:
            if self.fail_accept:
                raise ConsoleError(f"injected failure for {self.ids[0]}")
            self.phase = ["active", "active"]
        elif command == gate_pair.REJECT:
            assert self.phase[i] == "incoming"
            self.phase[i], self.gens[i], self.peer[i] = "idle", 0, "-"
        elif command == gate_pair.HANGUP:
            assert self.phase[i] != "incoming"
            complete = [i]
            if not (self.keep_callee_ringing or self.foreign_incoming or self.foreign_wechat):
                complete.append(1 - i)
            for endpoint in complete:
                self.phase[endpoint], self.gens[endpoint], self.peer[endpoint] = "idle", 0, "-"
        else:
            raise AssertionError(command)
        return ["OK"]

    def run(self):
        def fast_poll(read, accept, timeout):
            for _ in range(4):
                result = read()
                if accept(result):
                    return result
            raise ConsoleError("timeout")

        with patch.object(gate_pair, "poll", fast_poll), patch.object(gate_pair.time, "sleep"):
            summary = gate_pair.run_pair(self.endpoint(0), self.endpoint(1))
        assert not any(device in json.dumps(summary) for device in self.ids)
        return summary


class PairGateTest(unittest.TestCase):
    def test_both_directions_use_distinct_generations_and_the_active_audio_slot(self):
        pair = Pair()
        summary = pair.run()
        self.assertEqual(summary["status"], "pass")
        self.assertEqual(pair.sent, [(0, f"AT+CALL DEVICE {pair.ids[1]}"), (1, gate_pair.ACCEPT),
                                     (0, gate_pair.HANGUP), (1, f"AT+CALL DEVICE {pair.ids[0]}"),
                                     (0, gate_pair.ACCEPT), (1, gate_pair.HANGUP)])
        forward, reverse = (summary["facts"][side] for side in ("left_to_right", "right_to_left"))
        self.assertEqual((forward["caller_generation"], forward["callee_generation"]), (11, 21))
        self.assertEqual((reverse["caller_generation"], reverse["callee_generation"]), (22, 12))
        self.assertEqual(forward["callee_audio"], {"audio_tx": 20, "audio_rx": 20,
                         "media_received": 20, "media_decoded_packets": 20, "pcm_written": 320, "write_failed": 0})
        self.assertEqual(pair.phase, ["idle", "idle"])

    def test_decoded_audio_without_successful_output_fails(self):
        pair = Pair()
        pair.no_output = 1
        summary = pair.run()
        self.assertEqual(summary["status"], "fail")
        self.assertIn("output", summary["detail"])
        self.assertEqual(pair.phase, ["idle", "idle"])

    def test_missing_output_counter_is_not_verified_before_dial(self):
        pair = Pair()
        pair.no_output_counter = True
        summary = pair.run()
        self.assertEqual(summary["status"], "not_verified")
        self.assertIn("playback counters", summary["detail"])
        self.assertFalse(pair.sent)

    def test_media_generation_must_match_the_connected_rtc_slot(self):
        pair = Pair()
        pair.foreign_media = True
        summary = pair.run()
        self.assertEqual(summary["status"], "fail")
        self.assertIn("media generation", summary["detail"])
        self.assertEqual(pair.phase, ["idle", "idle"])

    def test_output_counter_reset_cannot_pass(self):
        pair = Pair()
        pair.reset_output_counter = True
        self.assertEqual(pair.run()["status"], "fail")
        self.assertEqual(pair.phase, ["idle", "idle"])

    def test_missing_reverse_dial_or_cleanup_grant_prevents_either_dial(self):
        for side, verb in ((1, gate_pair.REJECT), (0, gate_pair.HANGUP), (1, "dial")):
            with self.subTest(side=side, verb=verb):
                pair = Pair()
                pair.grants[side].remove(f"AT+CALL DEVICE {pair.ids[1 - side]}" if verb == "dial" else verb)
                self.assertEqual(pair.run()["status"], "not_verified")
                self.assertFalse(pair.sent)

    def test_usb_mismatch_prevents_any_write(self):
        pair = Pair()
        pair.confirmable[1] = False
        self.assertEqual(pair.run()["status"], "not_verified")
        self.assertFalse(pair.sent)

    def test_usb_change_after_final_read_prevents_dial(self):
        pair = Pair()
        pair.lose_usb_after_last_dial_probe = True
        self.assertEqual(pair.run()["status"], "not_verified")
        self.assertFalse(pair.sent)

    def test_non_open_or_missing_slot_state_does_not_count_as_active_audio(self):
        for slot_state in (0, 1, None):
            with self.subTest(slot_state=slot_state):
                pair = Pair()
                pair.slot_state = slot_state
                summary = pair.run()
                self.assertEqual(summary["status"], "fail")
                self.assertIn("connected RTC slot", summary["detail"])
                self.assertEqual(summary["facts"]["left_to_right"],
                                 {"caller_idle_verified": True, "callee_idle_verified": True})

    def test_no_accept_or_cleanup_on_wrong_incoming_peer_or_other_provider(self):
        for property_name in ("foreign_incoming", "foreign_wechat"):
            with self.subTest(conflict=property_name):
                pair = Pair()
                setattr(pair, property_name, True)
                self.assertEqual(pair.run()["status"], "fail")
                self.assertEqual(pair.sent, [(0, f"AT+CALL DEVICE {pair.ids[1]}"), (0, gate_pair.HANGUP)])

    def test_known_ringing_callee_uses_reject_after_failed_accept(self):
        pair = Pair()
        pair.fail_accept = pair.keep_callee_ringing = True
        summary = pair.run()
        self.assertEqual(summary["status"], "fail")
        self.assertEqual(pair.sent[-3:], [(1, gate_pair.ACCEPT), (0, gate_pair.HANGUP), (1, gate_pair.REJECT)])
        self.assertTrue(summary["facts"]["left_to_right"]["callee_idle_verified"])

    def test_reused_generation_and_reappearance_after_idle_are_not_owned(self):
        for property_name in ("reuse_gen", "reappear"):
            with self.subTest(name=property_name):
                pair = Pair()
                setattr(pair, property_name, True)
                self.assertEqual(pair.run()["status"], "fail")
                self.assertEqual([cmd for _, cmd in pair.sent if cmd == gate_pair.HANGUP], [gate_pair.HANGUP])

    def test_no_audio_fails_and_leaves_both_verified_idle(self):
        pair = Pair()
        pair.missing_audio = True
        summary = pair.run()
        self.assertEqual(summary["status"], "fail")
        self.assertEqual(summary["facts"]["left_to_right"],
                         {"caller_idle_verified": True, "callee_idle_verified": True})

    def test_idle_during_audio_retires_the_generation_before_error_cleanup(self):
        pair = Pair()
        pair.reappear_during_audio = True
        summary = pair.run()
        self.assertEqual(summary["status"], "fail")
        self.assertNotIn((0, gate_pair.HANGUP), pair.sent)
        self.assertEqual(summary["facts"]["left_to_right"],
                         {"caller_idle_verified": False, "callee_idle_verified": True})


if __name__ == "__main__":
    unittest.main()
