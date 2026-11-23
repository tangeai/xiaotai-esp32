"""Offline AI gate tests using the SDK session and a generated-vocabulary-shaped fixture."""

from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import gate_ai
from console_sdk import ConsoleError, Session, Vocabulary, _PLATFORM_STATS_KEYS


SHA = "87" * 32
START, STOP, STATUS, LINK, PLATFORM = (gate_ai.START, gate_ai.STOP, gate_ai.STATUS, gate_ai.LINK, gate_ai.PLATFORM)
MEDIA = gate_ai.MEDIA


class Wire:
    def __init__(self):
        read = ("AT", "AT+VERSION", "AT+STATE", "AT+LINK", "AT+PLATFORM")
        commands = [dict(command=name, usage="", access="read") for name in read]
        commands.append(dict(command="AT+STATUS", usage="[<section>]", access="read"))
        commands.extend(dict(command="AT+AI", verb=verb, usage="", access="action") for verb in ("START", "STOP"))
        self.vocab = Vocabulary({"product": "ai-fixture", "commands": commands}, board="fixture")
        self.clock = 10.0
        self.calls = []
        self.active = False
        self.started = 5
        self.phase = "idle"
        self.end = "none"
        self.gen = 0
        self.link = 0
        self.foreign_at = None
        self.transient_foreign_at = None
        self.natural_end_at = None
        self.change_end_at = None
        self.fail_confirm = None
        self.confirmed = 0
        self.witnesses = 0
        self.fail_witness = None
        self.mono = False
        self.offline = False
        self.media = dict(ready=1, active=0, draining=0, uplink_open=0, generation=0, playing=0, recording=0)
        self.media_settles_at = None
        self.foreign_while_waiting = False
        self.offline_while_waiting = False
        self.lost_start = False
        self.lost_stop = False

    def confirm(self):
        self.confirmed += 1
        self.calls.append("confirm")
        if self.fail_confirm == self.confirmed:
            raise ConsoleError("USB identity changed")

    def checkpoint(self, idle=False):
        self.witnesses += 1
        if self.fail_witness == self.witnesses or (idle and self.active):
            raise ConsoleError("not the original idle boot")

    def sleep(self, delay):
        self.clock += delay

    def exchange(self, command, wait):
        self.calls.append(command)
        self.clock += 0.02
        if command == "AT+VERSION":
            return ["+VERSION: board=fixture chip=esp32s3 at=1", f"+BUILD: elf_sha256={SHA}", "OK"]
        if command == "AT+HELP":
            return [f"+HELP: {entry.label}" for entry, _ in self.vocab.entries] + ["OK"]
        if command == PLATFORM:
            stats = ["+PLATFORM-STATS: " + " ".join(f"{key}=0" for key in _PLATFORM_STATS_KEYS[start:start + 9])
                     for start in range(0, len(_PLATFORM_STATS_KEYS), 9)]
            return [f"+PLATFORM: online={int(not self.offline)}", *stats, "OK"]
        if command == MEDIA:
            if self.calls.count(MEDIA) == self.media_settles_at:
                self.media.update(ready=1, playing=0)
                if self.foreign_while_waiting:
                    self.started += 1
                if self.offline_while_waiting:
                    self.offline = True
            return ["+MEDIA: " + " ".join(f"{key}={value}" for key, value in self.media.items()), "OK"]
        if command == STATUS:
            if self.natural_end_at == self.calls.count(STATUS) and self.active:
                self.active, self.phase, self.gen, self.link, self.end = False, "idle", 0, 0, "remote"
            if self.change_end_at == self.calls.count(STATUS) and not self.active:
                self.end = "timeout"
            if self.foreign_at == self.calls.count(STATUS) and self.active:
                self.started += 1
                self.gen, self.link = 102, 250
            if self.transient_foreign_at == self.calls.count(STATUS) and self.active:
                return ["+AI_TALK: phase=listening gen=102 link=250 origin=user end=none",
                        f"+AI-STATS: started={self.started + 1}", "OK"]
            row = f"+AI_TALK: phase={self.phase} gen={self.gen} link={self.link} origin=user end={self.end}"
            if self.phase == "connecting":
                self.phase = "listening"
            return [row, f"+AI-STATS: started={self.started}", "OK"]
        if command == LINK:
            return ["+LINK: started=1", "+LINK-SLOT: gen=31 audio_tx=999 audio_rx=999",
                    f"+LINK-SLOT: gen=200 audio_tx={int(self.active) * 3} audio_rx={int(self.active and not self.mono) * 2}",
                    "OK"]
        if command == START:
            self.active, self.phase, self.gen, self.link = True, "connecting", 101, 200
            self.started += 1
            return [] if self.lost_start else ["OK"]
        if command == STOP:
            self.active, self.phase, self.gen, self.link, self.end = False, "idle", 0, 0, "local"
            return [] if self.lost_stop else ["OK"]
        raise AssertionError(command)

    def run(self, permits=(START, STOP)):
        session = Session(self.exchange, self.vocab, permits, SHA)
        with (patch.object(gate_ai.time, "monotonic", side_effect=lambda: self.clock),
              patch.object(gate_ai.time, "sleep", side_effect=self.sleep)):
            return gate_ai.run_one(self, session, self, self.confirm)


class GateAiTest(unittest.TestCase):
    def test_one_round_proves_both_directions_and_local_idle(self):
        wire = Wire()
        result = wire.run()
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["facts"]["generation"], 101)
        self.assertEqual(result["facts"]["link_generation"], 200)
        self.assertEqual(result["facts"]["audio"], {"audio_tx": 3, "audio_rx": 2})
        self.assertTrue(result["facts"]["idle_verified"])
        self.assertEqual([call for call in wire.calls if call in (START, STOP, "confirm")],
                         ["confirm", START, "confirm", STOP])
        self.assertEqual(wire.calls[wire.calls.index(START) - 1], "confirm")
        self.assertEqual(wire.calls[wire.calls.index(STOP) - 2:wire.calls.index(STOP)], ["confirm", STATUS])

    def test_hold_missing_permission_identity_or_online_before_start(self):
        for condition in ("permit", "usb", "app", "platform"):
            with self.subTest(condition=condition):
                wire = Wire()
                wire.fail_confirm = 1 if condition == "usb" else None
                wire.active = condition == "app"
                wire.offline = condition == "platform"
                result = wire.run((START,) if condition == "permit" else (START, STOP))
                self.assertEqual(result["status"], "not_verified")
                self.assertFalse(result["facts"]["start_attempted"])
                self.assertNotIn(START, wire.calls)
                self.assertNotIn(STOP, wire.calls)

    def test_waits_for_end_cue_then_rechecks_boot_and_ai_before_start(self):
        wire = Wire()
        wire.media["playing"] = 3
        wire.media_settles_at = 3
        result = wire.run()
        self.assertEqual(result["status"], "pass")
        self.assertEqual(wire.calls[:wire.calls.index(START)].count(MEDIA), 3)
        self.assertEqual(result["facts"]["media_before_start"], wire.media)
        self.assertEqual(wire.witnesses, 4)

    def test_persistent_media_fault_or_conflict_sends_no_action(self):
        for keys in (dict(ready=0), dict(ready=2), dict(active=1), dict(draining=1),
                     dict(uplink_open=1), dict(generation=19), dict(playing=3), dict(recording=4)):
            with self.subTest(media=keys):
                wire = Wire()
                wire.media.update(keys)
                result = wire.run()
                self.assertEqual(result["status"], "not_verified")
                self.assertFalse(result["facts"]["start_attempted"])
                self.assertNotIn(START, wire.calls)
                self.assertNotIn(STOP, wire.calls)

    def test_settled_media_cannot_hide_reboot_or_intervening_ai(self):
        for fault in ("boot", "foreign AI", "offline"):
            with self.subTest(fault=fault):
                wire = Wire()
                wire.media["playing"] = 3
                wire.media_settles_at = 3
                wire.foreign_while_waiting = fault == "foreign AI"
                wire.offline_while_waiting = fault == "offline"
                wire.fail_witness = 2 if fault == "boot" else None
                result = wire.run()
                self.assertEqual(result["status"], "not_verified")
                self.assertNotIn(START, wire.calls)
                self.assertNotIn(STOP, wire.calls)

    def test_one_sided_audio_is_a_failure_but_owned_room_returns_idle(self):
        wire = Wire()
        wire.mono = True
        result = wire.run()
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["facts"]["audio"], {"audio_tx": 3, "audio_rx": 0})
        self.assertTrue(result["facts"]["idle_verified"])
        self.assertEqual(wire.calls.count(STOP), 1)

    def test_no_stop_for_lost_start_wrong_generation_or_lost_boot(self):
        for fault in ("lost start", "first generation replaced", "generation replaced before stop", "boot", "USB"):
            with self.subTest(fault=fault):
                wire = Wire()
                wire.lost_start = fault == "lost start"
                wire.foreign_at = {"first generation replaced": 2, "generation replaced before stop": 5}.get(fault)
                wire.fail_witness = 2 if fault == "boot" else None
                wire.fail_confirm = 2 if fault == "USB" else None
                result = wire.run()
                self.assertEqual(result["status"], "not_verified")
                self.assertTrue(result["facts"]["start_attempted"])
                self.assertFalse(result["facts"]["stop_attempted"])
                self.assertNotIn(STOP, wire.calls)

    def test_lost_ownership_is_not_reclaimed_during_cleanup(self):
        wire = Wire()
        wire.transient_foreign_at = 4  # After claim, before the first audio sample.
        result = wire.run()
        self.assertEqual(result["status"], "not_verified")
        self.assertTrue(result["facts"]["ownership_lost"])
        self.assertTrue(result["facts"]["cleanup_unverified"])
        self.assertEqual(wire.gen, 101)  # A further snapshot would appear to match again.
        self.assertEqual(wire.calls.count(STATUS), 4)
        self.assertNotIn(STOP, wire.calls)

    def test_failed_stop_ack_does_not_trigger_second_stop(self):
        wire = Wire()
        wire.lost_stop = True
        result = wire.run()
        self.assertEqual(result["status"], "not_verified")
        self.assertEqual(wire.calls.count(STOP), 1)
        self.assertTrue(result["facts"]["stop_attempted"])
        self.assertFalse(result["facts"]["stop_acked"])

    def test_keeps_end_reason_from_verified_idle_without_sending_stop(self):
        for phase_at_end in (2, 3, 4, 5):
            with self.subTest(status_query=phase_at_end):
                wire = Wire()
                wire.natural_end_at = phase_at_end
                result = wire.run()
                self.assertEqual(result["status"], "fail")
                self.assertEqual(result["facts"]["terminal_end"], "remote")
                self.assertTrue(result["facts"]["idle_verified"])
                self.assertFalse(result["facts"].get("cleanup_unverified", False))
                self.assertNotIn(STOP, wire.calls)

    def test_early_end_without_same_boot_witness_does_not_enable_stop(self):
        wire = Wire()
        wire.natural_end_at = 4
        wire.fail_witness = 2
        result = wire.run()
        self.assertEqual(result["status"], "not_verified")
        self.assertNotIn("terminal_end", result["facts"])
        self.assertNotIn(STOP, wire.calls)

    def test_disagreeing_terminal_rows_do_not_publish_an_end_reason(self):
        wire = Wire()
        wire.natural_end_at = 4
        wire.change_end_at = 5
        result = wire.run()
        self.assertEqual(result["status"], "not_verified")
        self.assertTrue(result["facts"]["ownership_lost"])
        self.assertNotIn("terminal_end", result["facts"])
        self.assertNotIn(STOP, wire.calls)


if __name__ == "__main__":
    unittest.main()
