"""Synthetic, credential-free Android UI pages; never contact adb or a board."""
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from xml.etree import ElementTree as ET

import xt
from phone_ui import Page, blocked_page


def dump(*labels, package="com.tencent.mm", field="text"):
    root = ET.Element("hierarchy")
    node = ET.SubElement(root, "node", {"package": package, "class": "android.webkit.WebView"})
    for label in labels:
        ET.SubElement(node, "node", {field: label, "package": package, "class": "android.widget.TextView"})
    return ET.tostring(root, encoding="utf-8")


EXPIRED = dump("小钛", "登录状态已过期，请重新登录", "重新登录")
EMPTY = dump("小钛", "添加设备", "添加你的第一台设备", field="content-desc")
LIST = dump("小钛", "我的设备 6", "添加设备", "演示板 A", "演示板 B")
UNRECOGNIZED = dump("小钛", "探鸽小钛 申请", "已开启提醒")


class PhoneUiTest(unittest.TestCase):
    def test_expired_and_empty_are_distinct_positive_markers(self):
        self.assertIs(blocked_page(EXPIRED), Page.LOGIN_EXPIRED)
        self.assertIs(blocked_page(EMPTY), Page.EMPTY_LIST)

    def test_nonempty_list_without_target_on_this_view_is_not_empty(self):
        self.assertIs(blocked_page(LIST), Page.UNKNOWN)

    def test_unknown_screens_are_not_taken_as_login_expired_or_empty(self):
        for data in (
            UNRECOGNIZED, dump("小钛", "注册", "安全验证"),
            dump("我的设备", "添加设备", "添加你的第一台设备"),
            dump("小钛", "登录状态已过期", package="com.android.chrome"),
            None, b"<hierarchy", b'<!DOCTYPE hierarchy []><hierarchy/>',
            b"<hierarchy>\xff</hierarchy>",
            '<!DOCTYPE hierarchy []><hierarchy/>'.encode("utf-16"),
            '<!DOCTYPE hierarchy []><hierarchy/>'.encode("utf-16-le"),
        ):
            with self.subTest(data=data):
                self.assertIs(blocked_page(data), Page.UNKNOWN)

    def test_preflight_stops_on_blockers_before_relaunch(self):
        for data, expected in ((EXPIRED, Page.LOGIN_EXPIRED), (EMPTY, Page.EMPTY_LIST)):
            with (self.subTest(expected=expected),
                  patch.object(xt, "phone_profile", return_value={}),
                  patch.object(xt, "read_phone_ui", return_value=data),
                  patch.object(xt, "phone_state", return_value="other"),
                  patch.object(xt.subprocess, "run") as adb,
                  patch.object(xt, "tap") as tap):
                self.assertIs(xt.reopen_miniprogram(), expected)
                adb.assert_not_called()
                tap.assert_not_called()

    def test_no_new_click_on_recognized_list(self):
        with (patch.object(xt, "phone_profile", return_value={}),
              patch.object(xt, "read_phone_ui") as ui,
              patch.object(xt, "phone_state", return_value="list"),
              patch.object(xt.subprocess, "run") as adb,
              patch.object(xt, "tap") as tap):
            self.assertIs(xt.reopen_miniprogram(), Page.READY)
            ui.assert_not_called()
            adb.assert_not_called()
            tap.assert_not_called()

    def test_unknown_page_after_bounded_recovery_stays_unknown(self):
        with (patch.object(xt, "phone_profile", return_value={"drawer_xiaotai": (1, 2)}),
              patch.object(xt, "read_phone_ui", return_value=UNRECOGNIZED) as ui,
              patch.object(xt, "phone_state", return_value="other"),
              patch.object(xt.time, "sleep"), patch.object(xt.subprocess, "run") as adb,
              patch.object(xt, "tap") as tap):
            self.assertIs(xt.reopen_miniprogram(), Page.UNKNOWN)
            self.assertEqual((ui.call_count, adb.call_count, tap.call_count), (2, 2, 1))

    def test_known_list_pixels_and_an_empty_list_without_a_card(self):
        profile = {"answer_banner": (1228, 374), "list_probe": (408, 961),
                   "add_button_probe": (1043, 395)}
        colors = {(100, 1600): (255, 255, 255), (408, 961): xt.GREEN, (1043, 395): xt.GREEN}
        with (patch.object(xt, "phone_profile", return_value=profile),
              patch.object(xt, "screen_pixels", return_value=lambda x, y: colors.get((x, y), (255, 255, 255)))):
            self.assertEqual(xt.phone_state(), "list")
            colors.pop((408, 961))
            self.assertEqual(xt.phone_state(), "other")

    def test_legacy_expiry_does_not_repeat_setup_in_cleanup(self):
        device = {"port": "fake", "kind": "cores3", "phone_wx_button": (1, 2)}
        with (patch.object(xt, "resolve", return_value=("fake", device)),
              patch.object(xt, "run_dir", return_value=xt.KIT),
              patch.object(xt, "Port"), patch.object(xt, "require_legacy"),
              patch.object(xt, "media"), patch.object(xt, "phone_profile"),
              patch.object(xt, "reopen_miniprogram", return_value=Page.LOGIN_EXPIRED) as reopen,
              patch.object(xt, "tap") as tap, patch.object(xt, "finish") as finish):
            xt.wx_run(SimpleNamespace(dev="fake"), incoming=True)
            self.assertEqual(reopen.call_count, 1)
            self.assertEqual(finish.call_args.args[1]["failure"], xt.phone_ui.FAILURE[Page.LOGIN_EXPIRED])
            tap.assert_not_called()


if __name__ == "__main__":
    unittest.main()
