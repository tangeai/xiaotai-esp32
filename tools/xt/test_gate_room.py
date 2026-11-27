"""Offline room gate tests; fake all console and USB operations."""

import unittest
from types import SimpleNamespace
from unittest.mock import patch

import xt  # noqa: F401 (puts scripts/ on the import path)
import gate_room as room
from console_sdk import ConsoleError


class Fixture:
    def __init__(self):
        self.events = []
        self.available = set(room.READS + room.ACTIONS)
        self.denied = set()
        self.phase, self.open, self.generation, self.revision = 3, 0, 0, 3
        self.activity, self.activity_id, self.tx = "none", 0, 0
        self.known = self.assigned = self.settled = self.online = True
        self.code, self.tx_growth = "123456", 8
        self.fail_after_press = None
        self.other_slot = None

    def has(self, command):
        return command in self.available

    def check(self, command):
        if command in self.denied:
            raise ConsoleError("missing exact permit")
        return SimpleNamespace(access="action" if command in room.ACTIONS else "read")

    def send(self, command):
        if command in room.ACTIONS and self.events[-1:] != ["confirmed"]:
            raise AssertionError("action was not immediately preceded by the USB identity check")
        self.events.append(command)
        if command == "AT+STATUS room":
            return [f"+ROOM: state={self.phase} code={self.code} known={int(self.known)} assigned={int(self.assigned)}"
                    f" open={self.open} busy=0 transmitting={int(self.phase == 6)} gen={self.generation}"
                    f" revision={self.revision} error=0", f"+ROOM-SESSION: settled={int(self.settled)}", "OK"]
        if command == "AT+STATE":
            return [f"+STATE: started=1 activity={self.activity} id={self.activity_id}", "OK"]
        if command == "AT+PLATFORM":
            return [f"+PLATFORM: online={int(self.online)} clock=1 discovered=1 authorized=1"
                    " signaling=1 verifying=0 held=0 gated=0", "OK"]
        if command == "AT+LINK":
            conn = int(self.generation != 0)
            lines = [f"+LINK: started=1 connected={conn}", "+LINK-MEDIA: tx=99"]
            if conn:
                generation = self.other_slot if self.other_slot is not None else self.generation
                lines.append(f"+LINK-SLOT: gen={generation} state=2 audio_tx={self.tx}")
            return lines + ["OK"]
        if command == room.OPEN:
            self.open, self.generation, self.activity, self.activity_id, self.phase = 1, 9, "room", 12, 5
        elif command == room.PRESS:
            self.phase = 6
            if self.fail_after_press:
                self.fail_after_press()
        elif command == room.RELEASE:
            self.phase = 5
            self.tx += self.tx_growth
        elif command == room.CLOSE:
            self.open, self.generation, self.activity, self.activity_id, self.phase = 0, 0, "none", 0, 3
        else:
            raise AssertionError(command)
        self.revision += 1
        return ["OK"]

    def checkpoint(self, idle=False):
        self.events.append("witness_idle" if idle else "witness")
        if idle and self.activity != "none":
            raise ConsoleError("not idle")

    def confirm(self):
        self.events.append("confirmed")

    @staticmethod
    def poll(read, accept, timeout):
        for _ in range(3):
            sample = read()
            if accept(sample):
                return sample
        raise ConsoleError("timed out")

    def run(self):
        with patch.object(room, "poll", self.poll):
            return room.run_one(None, self, self, self.confirm)

    @property
    def actions(self):
        return [event for event in self.events if event in room.ACTIONS]


class RoomGateTest(unittest.TestCase):
    def test_assigned_room_uses_its_own_audio_slot_not_global_counter(self):
        f = Fixture()
        result = f.run()
        self.assertEqual(f.actions, list(room.ACTIONS))
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["facts"], {"audio_tx_delta": 8, "closed": True, "membership_unchanged": True})
        self.assertNotIn(f.code, repr(result))
        self.assertIn("remote listening not verified", result["detail"])

    def test_no_action_for_unknown_unassigned_busy_or_offline_room(self):
        for case in (dict(known=False), dict(assigned=False, code="-"), dict(open=1),
                     dict(activity="ai_talk", activity_id=30), dict(settled=False), dict(online=False)):
            with self.subTest(case=case):
                f = Fixture()
                f.__dict__.update(case)
                self.assertEqual(f.run()["status"], "not_verified")
                self.assertEqual(f.actions, [])

    def test_cleanup_controls_must_be_present_and_authorized_before_any_action(self):
        f = Fixture()
        f.available.remove(room.CLOSE)
        self.assertEqual(f.run()["status"], "not_verified")
        self.assertEqual(f.actions, [])
        f.available.add(room.CLOSE)
        f.denied.add(room.RELEASE)
        with self.assertRaisesRegex(ConsoleError, "exact permit"):
            f.run()
        self.assertEqual(f.actions, [])

    def test_foreign_generation_never_receives_release_or_close(self):
        f = Fixture()
        f.fail_after_press = lambda: setattr(f, "generation", 10)
        with self.assertRaisesRegex(ConsoleError, "generation changed.+cleanup"):
            f.run()
        self.assertEqual(f.actions, [room.OPEN, room.PRESS])

    def test_open_can_refresh_existing_assignment_before_the_new_link_joins(self):
        f = Fixture()
        original = f.send

        def send(command):
            if command == room.OPEN:
                lines = original(command)
                f.known, f.phase, f.generation = False, 1, 0
                return lines
            if command == "AT+STATUS room" and f.open and not f.known:
                lines = original(command)
                f.known, f.phase, f.generation = True, 5, 9
                return lines
            return original(command)

        f.send = send
        self.assertEqual(f.run()["status"], "pass")
        self.assertEqual(f.actions, list(room.ACTIONS))

    def test_missing_own_audio_slot_closes_only_attendance(self):
        f = Fixture()
        f.other_slot = 10
        with self.assertRaisesRegex(ConsoleError, "link slot"):
            f.run()
        self.assertEqual(f.actions, [room.OPEN, room.CLOSE])

    def test_valid_owned_room_with_no_packets_is_a_failed_measurement(self):
        f = Fixture()
        f.tx_growth = 0
        self.assertEqual(f.run()["status"], "fail")
        self.assertEqual(f.actions, list(room.ACTIONS))

    def test_usb_loss_prohibits_release_and_close(self):
        f = Fixture()
        original = f.confirm

        def fail_confirm():
            if room.PRESS in f.events:
                raise ConsoleError("USB identity no longer matches")
            original()

        f.confirm = fail_confirm
        with self.assertRaisesRegex(ConsoleError, "USB identity.+cleanup"):
            f.run()
        self.assertEqual(f.actions, [room.OPEN, room.PRESS])


if __name__ == "__main__":
    unittest.main()
