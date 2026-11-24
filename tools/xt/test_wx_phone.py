"""Host-only visual and XML counterexamples for WeChat phone checks."""
import io
import tempfile
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
from xml.etree import ElementTree as ET

try:
    from PIL import Image
except ImportError:
    Image = None

import xt
from test_phone_ui import EMPTY, EXPIRED, LIST
from wx_phone import VerifiedIncomingPhone, VerifiedPhone, ended_seconds, live_hangup_at


CALLER = "示例板摄像头"
CONTROLS = {"clickable": "true", "enabled": "true", "bounds": "[7,5][12,10]"}


def dump(*labels, package="com.tencent.mm", control=None):
    root = ET.Element("hierarchy")
    for value in labels:
        ET.SubElement(root, "node", {"text": value, "package": package})
    if control:
        ET.SubElement(root, "node", {"text": "挂断", "package": package, **control})
    return ET.tostring(root, encoding="utf-8")


def receipt(caller=CALLER, duration="00:10", title="通话已结束", **options):
    return dump("探鸽小钛", title, "通话对象", caller, "通话类型", "语音通话", "通话时长", duration, **options)


@unittest.skipIf(Image is None, "Pillow is optional outside phone media tests")
class WxPhoneTemplateTest(unittest.TestCase):
    def setUp(self):
        directory = self.enterContext(tempfile.TemporaryDirectory())
        self.profile = {"adb_serial": "selected", "answer_banner": (9, 6),
                        "wx_caller_box": (0, 0, 6, 4), "wx_controls_box": (6, 0, 12, 8),
                        "list_probe": (2, 2), "add_button_probe": (4, 2)}
        self.banner = Image.new("RGB", (12, 10), (64, 64, 64))
        self.banner.putpixel((3, 2), (255, 255, 255))
        self.banner.putpixel((9, 6), (88, 190, 106))
        self.banner.save(Path(directory) / "banner.png")
        self.phone = VerifiedPhone(self.profile, {"caller": CALLER, "banner_png": "banner.png"}, directory)

    def test_other_callers_and_controls_fail_even_with_the_same_green_pixel(self):
        self.assertTrue(self.phone.matches(self.banner))
        for point in ((3, 2), (6, 5)):
            candidate = self.banner.copy()
            candidate.putpixel(point, (22, 24, 26))
            self.assertFalse(self.phone.matches(candidate))
        self.assertFalse(self.phone.matches(self.banner.resize((24, 20))))

    def test_fresh_verified_banner_allows_only_one_answer_on_selected_phone(self):
        other = self.banner.copy()
        other.putpixel((3, 2), (109, 109, 109))
        with (patch.object(self.phone, "frame", side_effect=[other, self.banner]) as frame,
              patch.object(xt.time, "sleep"), patch.object(xt.subprocess, "run",
                                                          return_value=SimpleNamespace(returncode=0, stdout=b"")) as run):
            self.assertTrue(self.phone.answer())
            self.assertEqual(frame.call_count, 2)
            self.assertEqual(run.call_args.args[0], ["adb", "-s", "selected", "shell", "input", "tap", "9", "6"])
            self.assertEqual(run.call_count, 1)

    def test_preexisting_banner_or_unknown_page_is_never_clicked_to_enter(self):
        for frame, error in ((self.banner, "already shows the expected call banner"),
                             (Image.new("RGB", self.banner.size, (50, 50, 50)), "unrecognized")):
            with (patch.object(xt, "phone_profile", return_value=self.profile),
                  patch.object(xt.wx_phone, "VerifiedPhone", return_value=self.phone),
                  patch.object(self.phone, "frame", return_value=frame),
                  patch.object(xt, "read_phone_ui", return_value=None),
                  patch.object(xt.subprocess, "run") as run):
                with self.assertRaisesRegex(xt.ConsoleError, error):
                    xt.outgoing_phone({})
                run.assert_not_called()

    def test_gate_phone_mismatch_stops_before_any_adb(self):
        with (patch.object(xt, "phone_profile", return_value=self.profile),
              patch.object(xt.wx_phone, "VerifiedPhone") as constructor,
              patch.object(xt.subprocess, "run") as run):
            with self.assertRaisesRegex(xt.ConsoleError, "authorized gate phone"):
                xt.outgoing_phone({}, expected_serial="different-phone")
            constructor.assert_not_called()
            run.assert_not_called()

    def test_gate_does_not_tap_if_banner_changed_during_authorization(self):
        changed = self.banner.copy()
        changed.putpixel((3, 2), (0, 0, 0))
        actions = []
        with (patch.object(self.phone, "frame", side_effect=[self.banner, changed]),
              patch.object(self.phone, "tap") as tap):
            self.assertFalse(self.phone.answer(confirm=actions.append))
            self.assertEqual(actions, ["PHONE_WX_OUT_ANSWER"])
            tap.assert_not_called()


class PhoneReceiptTest(unittest.TestCase):
    def test_receipt_requires_exact_peer_and_nonzero_duration(self):
        self.assertEqual(ended_seconds(receipt(), CALLER), 10)
        for sample in (receipt(caller="另一块示例板"), receipt(duration="00:00"),
                       receipt(duration="01:60"), receipt(title="正在呼叫"),
                       receipt(package="outside.app"), b"<broken"):
            self.assertIsNone(ended_seconds(sample, CALLER))

    def test_phone_hangup_only_with_the_live_expected_peer_and_enabled_control(self):
        active = dump("探鸽小钛", CALLER, "00:12", control=CONTROLS)
        self.assertTrue(live_hangup_at(active, CALLER, (9, 6)))
        self.assertFalse(live_hangup_at(active, CALLER, (1, 1)))
        self.assertFalse(live_hangup_at(active, "另一块示例板", (9, 6)))
        disabled = dump("探鸽小钛", CALLER, "00:12", control={**CONTROLS, "clickable": "false"})
        self.assertFalse(live_hangup_at(disabled, CALLER, (9, 6)))
        self.assertFalse(live_hangup_at(receipt(), CALLER, (9, 6)))


@unittest.skipIf(Image is None, "Pillow is optional outside phone media tests")
class IncomingTargetTest(unittest.TestCase):
    foreground = b"topResumedActivity=ActivityRecord{36 u0 com.tencent.mm/.plugin.appbrand.ui.AppBrandUI01 t12}"

    def setUp(self):
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.reference = {"device_id": "dev-test", "adb_serial": "phone-selected", "list_png": "list.png",
                          "card_box": (0, 11, 26, 23),
                          "identity_box": (1, 12, 11, 21), "call_box": (17, 12, 24, 21),
                          "account_box": (2, 2, 13, 10)}
        self.read_ui = Mock(return_value=None)
        self.profile = {"adb_serial": "phone-selected"}
        self.screen = Image.new("RGB", (32, 40), (230, 230, 230))
        for i, (x, y) in enumerate(((3, 15), (18, 16), (5, 3))):
            self.screen.putpixel((x, y), (18 + i, 21, 39))
            self.screen.putpixel((x + 1, y), (20, 163, 82))
        self.screen.save(self.directory / "list.png")

    def phone(self, profile=None, reference=None):
        return VerifiedIncomingPhone(self.profile if profile is None else profile,
                                     self.reference if reference is None else reference,
                                     self.directory, "dev-test")

    def adb(self, frames, *, foreground=None, returncode=0, point=(20, 16), ui=None):
        frames = iter(frames)
        views = iter(foreground) if isinstance(foreground, list) else None

        def run(argv, *, capture_output, timeout):
            self.assertEqual(argv[:3], ["adb", "-s", "phone-selected"])
            self.assertTrue(capture_output)
            if len(argv) == 5 and argv[3] == "shell":
                self.assertEqual(timeout, 6)
                self.assertTrue(argv[4].startswith('umask 077; trap "rm -f /data/local/tmp/xt-ui-'))
                self.assertIn("uiautomator dump /data/local/tmp/xt-ui-", argv[4])
                return SimpleNamespace(returncode=int(ui is None), stdout=ui or b"")
            self.assertEqual(timeout, 3)
            if argv[3:6] == ["shell", "dumpsys", "activity"]:
                screen = next(views) if views is not None else self.foreground if foreground is None else foreground
                return SimpleNamespace(returncode=returncode, stdout=screen)
            if argv[3:] == ["exec-out", "screencap", "-p"]:
                data = io.BytesIO()
                next(frames).save(data, format="PNG")
                return SimpleNamespace(returncode=0, stdout=data.getvalue())
            self.assertEqual(argv[3:], ["shell", "input", "tap", *map(str, point)])
            return SimpleNamespace(returncode=0, stdout=b"")

        return patch("wx_phone.subprocess.run", side_effect=run)

    def test_verified_row_twice_dials_once_with_pinned_serial(self):
        checks = []
        with self.adb([self.screen, self.screen]) as run:
            self.phone().dial(lambda: checks.append(run.call_count), read_ui=self.read_ui)
        self.assertEqual(checks, [2])
        self.assertEqual(run.call_count, 5)
        self.read_ui.assert_not_called()

    def test_portrait_device_identity_above_button_in_same_card(self):
        screen = Image.new("RGB", (1440, 3168), (230, 230, 230))
        for i, (x, y) in enumerate(((108, 1470), (282, 1670), (1000, 300))):
            screen.putpixel((x, y), (18 + i, 21, 39))
            screen.putpixel((x + 1, y), (20, 163, 82))
        screen.save(self.directory / "portrait.png")
        boxes = {"list_png": "portrait.png", "card_box": (57, 1290, 1380, 1880),
                 "identity_box": (90, 1410, 410, 1480), "call_box": (100, 1610, 500, 1680),
                 "account_box": (920, 250, 1330, 320)}
        with self.adb([screen, screen], point=(300, 1645)) as run:
            self.phone(reference={**self.reference, **boxes}).dial(lambda: None, read_ui=self.read_ui)
        self.assertEqual(run.call_count, 5)
        self.read_ui.assert_not_called()

    def test_crops_must_be_inside_one_valid_card_and_account_outside(self):
        for boxes in ({"card_box": (0, 11, 23, 23)}, {"card_box": (0, 11, 26, 41)},
                      {"call_box": (10, 12, 24, 21)}, {"account_box": (5, 17, 12, 23)}):
            with self.subTest(boxes=boxes):
                with self.assertRaisesRegex(xt.ConsoleError, "configure a reviewed device card"):
                    self.phone(reference={**self.reference, **boxes})

    def test_empty_other_row_wrong_account_disabled_button_and_duplicate_stop(self):
        empty = Image.new("RGB", self.screen.size, (230, 230, 230))
        candidates = [empty]
        for key in ("identity_box", "account_box"):
            duplicate = self.screen.copy()
            duplicate.paste(self.screen.crop(self.reference[key]), (1, 30))
            candidates.append(duplicate)
        for changed in ((3, 15), (18, 16), (5, 3)):
            frame = self.screen.copy()
            frame.putpixel(changed, (255, 100, 0))
            candidates.append(frame)
        for frame in candidates:
            with self.subTest(frame=frame.tobytes()[0:4]):
                with self.adb([frame]) as run:
                    with self.assertRaisesRegex(xt.ConsoleError, "absent or ambiguous"):
                        self.phone().dial(lambda: self.fail("wrong view cannot be authorized"), read_ui=self.read_ui)
                self.assertEqual(run.call_count, 2)

    def test_unverified_card_uses_only_pinned_read_only_ui_for_explicit_blockers(self):
        empty = Image.new("RGB", self.screen.size, (230, 230, 230))
        for dump, problem in ((EXPIRED, "login expired"), (EMPTY, "device list is empty"),
                              (LIST, "absent or ambiguous"), (None, "absent or ambiguous")):
            with self.subTest(problem=problem):
                with self.adb([empty], ui=dump) as run:
                    with self.assertRaisesRegex(xt.ConsoleError, problem):
                        self.phone().dial(lambda: self.fail("unverified view cannot be authorized"), read_ui=xt.read_phone_ui)
                self.assertEqual(run.call_count, 3)

    def test_second_snapshot_catches_row_or_app_switch(self):
        changed = self.screen.copy()
        changed.putpixel((3, 15), (255, 255, 0))
        with self.adb([self.screen, changed]) as run:
            with self.assertRaisesRegex(xt.ConsoleError, "absent or ambiguous"):
                self.phone().dial(lambda: None, read_ui=self.read_ui)
        self.assertEqual(run.call_count, 4)
        views = [self.foreground, b"topResumedActivity=ActivityRecord{1 other.app/.Home}"]
        with self.adb([self.screen], foreground=views) as run:
            with self.assertRaisesRegex(xt.ConsoleError, "not on the reviewed mini program"):
                self.phone().dial(lambda: None, read_ui=self.read_ui)
        self.assertEqual(run.call_count, 3)

    def test_unsupported_reference_ambiguous_phone_and_adb_failure_never_tap(self):
        with patch("wx_phone.subprocess.run") as run:
            for profile, reference in (({}, self.reference), ({"adb_serial": "other-phone"}, self.reference),
                                       (self.profile, {}), (self.profile, {**self.reference, "device_id": "other"}),
                                       (self.profile, {**self.reference, "account_box": (2, 22, 13, 31)}),
                                       (self.profile, {k: v for k, v in self.reference.items() if k != "card_box"})):
                with self.subTest(profile=profile, reference=bool(reference)):
                    with self.assertRaisesRegex(xt.ConsoleError, "configure a reviewed"):
                        self.phone(profile, reference)
            run.assert_not_called()
        with self.adb([], returncode=1) as run:
            with self.assertRaisesRegex(xt.ConsoleError, "selected phone rejected"):
                self.phone().dial(lambda: self.fail("ADB failure cannot be authorized"), read_ui=self.read_ui)
        self.assertEqual(run.call_count, 1)


if __name__ == "__main__":
    unittest.main()
