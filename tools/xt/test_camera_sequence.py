# SPDX-License-Identifier: MIT
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from camera_sequence import AI_START, AI_STATUS, AI_STOP, PROBE, VISION, Sequence, summarize
from console_sdk import ConsoleError
from test_camera_evidence import FRAME, LUMA, STATUS, TERMINAL


SHA = "ac" * 32


class Bus:
    def __init__(self):
        self.log = [FRAME.replace("0000ff1e", "11111111"), TERMINAL]
        self.sdk_start = 0
        self.actions = []
        self.permits = {PROBE, AI_START, AI_STOP}
        self.usable = True
        self.now = 100.0
        self.active = False
        self.generation = 21
        self.link = 7
        self.started = 13
        self.engine_online = True
        self.vision_enabled = True
        self.video_ok = True
        self.action_ack = True
        self.on_camera_sample = lambda: None
        self.on_checkpoint = lambda: None
        self.on_stop_confirm = lambda: None
        self.probe_lines = (FRAME, LUMA, TERMINAL)
        self.not_available = set()
        self.stop_releases = True
        self.usb_confirmed = True
        self.video_cleanup_active = False

    def sleep(self, delay):
        self.now += delay

    def has(self, command):
        return command not in self.not_available

    def check(self, command):
        return SimpleNamespace(access="action" if command in (PROBE, AI_START, AI_STOP) else "read")

    def confirm(self):
        if not self.usb_confirmed:
            raise ConsoleError("USB identity changed")
        if self.active:
            self.on_stop_confirm()

    def checkpoint(self, idle):
        self.on_checkpoint()
        if idle and self.active:
            raise ConsoleError("expected idle")
        return {"elf_sha256": SHA, "uptime_ms": int(self.now * 1000)}

    def since(self, mark):
        return self.log[mark:]

    def reply_video(self):
        if self.active or self.video_cleanup_active:
            self.on_camera_sample()
            if self.video_ok:
                return list(STATUS)
            return [STATUS[0], STATUS[1].replace("phase=running error=0x0", "phase=error error=TIMEOUT"),
                    STATUS[2], "OK"]
        return ["+VIDEO: requested=0 generation=0", "+VIDEO-UP: phase=off error=0x0 captured=0 sent=0 "
                "capture_failed=0 encode_failed=0 send_failed=0", "+VIDEO-DOWN: phase=off", "OK"]

    def send(self, command):
        if command == "AT+VIDEO":
            return self.reply_video()
        if command == "AT+STATE":
            return ["+STATE: activity=ai id=4" if self.active else "+STATE: activity=none id=0", "OK"]
        if command == AI_STATUS:
            return [f"+AI_TALK: phase=listening gen={self.generation} link={self.link} origin=user" if self.active
                    else "+AI_TALK: phase=idle gen=0 link=0 origin=user", f"+AI-STATS: started={self.started}", "OK"]
        if command == VISION:
            return [f"+AI_VISION: enabled={int(self.vision_enabled)} "
                    f"state={2 if self.active else 0} link={self.link if self.active else 0}", "OK"]
        if command == "AT+NET":
            return ["+NET: connected=1", "OK"]
        if command == "AT+PLATFORM":
            return ["+PLATFORM: online=1", "OK"]
        if command == "AT+LINK":
            return [f"+LINK: started={int(self.engine_online)} connected={int(self.active)}", "OK"]
        self.actions.append(command)
        if command == PROBE:
            self.sdk_start = len(self.log)
            self.log.extend(("I: old unrelated log", "OK", *self.probe_lines))
            return ["I: old unrelated log", "OK"]
        if command == AI_START:
            self.active = True
            self.started = (self.started + 1) & 0xffffffff
            if not self.action_ack:
                self.usable = False
                raise ConsoleError("timed out before the final serial reply")
        elif command == AI_STOP and self.stop_releases:
            self.active = False
        return ["OK"]

    def runner(self):
        return Sequence(self, self, self, self.confirm, 10, clock=lambda: self.now, sleep=self.sleep)


class CameraSequenceTest(unittest.TestCase):
    def test_requires_exact_local_actions_before_running(self):
        bus = Bus()
        bus.permits = {"AT+VIDEO", AI_START}
        report = summarize(["probe", "ai-vision"], 2, bus.runner())
        self.assertEqual(report["verdict"], "unverified")
        self.assertTrue(all("not authorized" in row["reason"] for row in report["rounds"]))
        self.assertFalse(bus.actions)
        bus.permits = {PROBE}
        bus.usb_confirmed = False
        self.assertEqual(bus.runner().run_once("probe")["verdict"], "fail")
        self.assertFalse(bus.actions)

    def test_probe_pass_never_grants_h5_and_only_accepts_fresh_wire(self):
        bus = Bus()
        report = summarize(["probe", "h5"], 2, bus.runner())
        self.assertEqual(report["verdict"], "unverified")
        self.assertEqual(report["coverage"], {"probe": "pass", "ai-vision": "unverified", "h5": "unverified"})
        self.assertEqual(bus.actions, [PROBE, PROBE])
        for row in report["rounds"][::2]:
            self.assertEqual((row["probe_frames"], row["first_frame"]["crc32"], row["first_luma"]["y_mean"]),
                             (3, "0000ff1e", 78))
            self.assertTrue(row["cleanup_verified"])
            self.assertIsNotNone(row["first_observed_ms"])
            self.assertIsNone(row["recovery"])

    def test_probe_with_only_old_terminal_never_retries(self):
        bus = Bus()
        bus.probe_lines = (FRAME,)
        report = summarize(["probe", "ai-vision"], 3, bus.runner())
        self.assertEqual(report["verdict"], "fail")
        self.assertEqual(bus.actions, [PROBE])
        self.assertIn("fresh terminal", report["rounds"][0]["reason"])
        self.assertIsNotNone(report["rounds"][0]["first_frame"])
        self.assertIsNone(report["rounds"][0]["cleanup_verified"])
        self.assertTrue(all(row["verdict"] == "unverified" for row in report["rounds"][1:]))

    def test_probe_failure_and_duplicate_terminal_do_not_continue(self):
        for lines in (("+VIDEO-PROBE: result=failed frames=0 error=TIMEOUT",), (FRAME, TERMINAL, TERMINAL)):
            with self.subTest(lines=lines):
                bus = Bus()
                bus.probe_lines = lines
                self.assertEqual(summarize(["probe"], 2, bus.runner())["verdict"], "fail")
                self.assertEqual(bus.actions, [PROBE])

    def test_unavailable_capabilities_and_busy_camera_are_never_changed(self):
        bus = Bus()
        bus.not_available = {PROBE}
        bus.vision_enabled = False
        self.assertEqual(summarize(["probe", "ai-vision"], 1, bus.runner())["verdict"], "unverified")
        self.assertFalse(bus.actions)
        bus.not_available.clear()
        bus.video_cleanup_active = True
        self.assertEqual(summarize(["probe"], 1, bus.runner())["verdict"], "fail")
        self.assertFalse(bus.actions)

    def test_ai_frame_is_scoped_to_activity_and_link_then_released(self):
        bus = Bus()
        report = summarize(["ai-vision"], 1, bus.runner())
        self.assertEqual(report["verdict"], "pass")
        row = report["rounds"][0]
        self.assertEqual((row["activity_generation"], row["link_generation"], row["first_frame"]["sent"]), (21, 7, 32))
        self.assertEqual(row["first_luma"]["y_mean"], 0)
        self.assertTrue(row["cleanup_verified"])
        self.assertEqual(bus.actions, [AI_START, AI_STOP])

    def test_unstarted_rtc_engine_never_starts_camera_ai(self):
        bus = Bus()
        bus.engine_online = False
        row = bus.runner().run_once("ai-vision")
        self.assertEqual(row["verdict"], "unverified")
        self.assertIn("RTC service", row["reason"])
        self.assertEqual(bus.actions, [])

    def test_ai_never_adopts_first_observed_replacement(self):
        bus = Bus()
        send = bus.send

        def rapid_replacement(command):
            reply = send(command)
            if command == AI_START:
                bus.started += 1
                bus.generation += 1
            return reply

        bus.send = rapid_replacement
        row = bus.runner().run_once("ai-vision")
        self.assertEqual(row["verdict"], "fail")
        self.assertNotIn("activity_generation", row)
        self.assertFalse(row["cleanup_verified"])
        self.assertEqual(bus.actions, [AI_START])

    def test_not_reacquired_after_loss_even_if_generation_reappears(self):
        bus = Bus()
        bus.on_camera_sample = lambda: setattr(bus, "generation", 22)
        bus.on_stop_confirm = lambda: setattr(bus, "generation", 21)
        row = bus.runner().run_once("ai-vision")
        self.assertEqual(row["verdict"], "fail")
        self.assertFalse(row["cleanup_verified"])
        self.assertEqual(bus.actions, [AI_START])

    def test_final_usb_confirmation_precedes_final_ai_ownership_read(self):
        bus = Bus()
        bus.on_stop_confirm = lambda: setattr(bus, "started", 15)
        row = bus.runner().run_once("ai-vision")
        self.assertEqual(row["verdict"], "fail")
        self.assertFalse(row["cleanup_verified"])
        self.assertEqual(bus.actions, [AI_START])

    def test_ai_video_failure_still_releases_owned_generation(self):
        bus = Bus()
        bus.video_ok = False
        report = summarize(["ai-vision", "probe"], 1, bus.runner())
        self.assertEqual(report["verdict"], "fail")
        self.assertEqual(report["rounds"][0]["video"]["error"], "TIMEOUT")
        self.assertTrue(report["rounds"][0]["cleanup_verified"])
        self.assertEqual(bus.actions, [AI_START, AI_STOP])

    def test_changed_ai_generation_is_never_stopped(self):
        bus = Bus()
        bus.on_camera_sample = lambda: setattr(bus, "generation", 22)
        report = summarize(["ai-vision", "probe"], 1, bus.runner())
        self.assertEqual(report["verdict"], "fail")
        row = report["rounds"][0]
        self.assertIn("owned AI generation", row["cleanup_failure"])
        self.assertFalse(row["cleanup_verified"])
        self.assertTrue(bus.active)
        self.assertEqual(bus.actions, [AI_START])

    def test_identity_change_and_lost_start_reply_never_send_stop(self):
        for confirmed in (False, True):
            with self.subTest(confirmed=confirmed):
                bus = Bus()
                bus.action_ack = not confirmed
                if not confirmed:
                    bus.on_camera_sample = lambda: setattr(bus, "usb_confirmed", False)
                row = summarize(["ai-vision"], 1, bus.runner())["rounds"][0]
                self.assertEqual(row["verdict"], "fail")
                self.assertFalse(row["cleanup_verified"])
                self.assertIn("unknown" if confirmed else "USB identity changed", row["cleanup_failure"])
                self.assertEqual(bus.actions, [AI_START])

    def test_incomplete_cleanup_times_out_without_a_second_stop(self):
        bus = Bus()
        bus.stop_releases = False
        report = summarize(["ai-vision"], 2, bus.runner())
        self.assertEqual(report["verdict"], "fail")
        self.assertIn("return to idle", report["rounds"][0]["cleanup_failure"])
        self.assertEqual(bus.actions, [AI_START, AI_STOP])

    def test_h5_does_not_contact_remote_or_send_any_at(self):
        def forbidden(_):
            self.fail("H5 must not invoke the serial runner")
        report = summarize(["h5"], 3, forbidden)
        self.assertEqual(report["verdict"], "unverified")
        for row in report["rounds"]:
            self.assertIn("no verified remote", row["reason"])
            self.assertIsNone(row["first_frame"])


if __name__ == "__main__":
    unittest.main()
