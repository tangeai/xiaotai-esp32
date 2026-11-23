"""Read phone-side evidence for WeChat bench tests on a selected handset."""
import io
from pathlib import Path
import re
import subprocess
import time

from phone_ui import FAILURE, Page, blocked_page, wechat_nodes
from console_sdk import ConsoleError


def label(node):
    return (node.get("text") or node.get("content-desc", "")).strip()


def ended_seconds(dump, caller):
    labels = [value for node in wechat_nodes(dump) if (value := label(node))]
    if labels.count("探鸽小钛") != 1 or labels.count("通话已结束") != 1:
        return None

    def after(title):
        return next((b for a, b in zip(labels, labels[1:]) if a == title), "") if labels.count(title) == 1 else ""

    if after("通话对象") != caller or after("通话类型") not in {"语音通话", "视频通话"}:
        return None
    duration = re.fullmatch(r"([0-9]{1,3}):([0-5][0-9])", after("通话时长"))
    if duration:
        return int(duration[1]) * 60 + int(duration[2]) or None
    return None


def live_hangup_at(dump, caller, point):
    nodes = wechat_nodes(dump)
    labels = [label(node) for node in nodes]
    if (labels.count("探鸽小钛") != 1 or labels.count(caller) != 1
            or "通话已结束" in labels or not any(re.fullmatch(r"[0-9]{1,3}:[0-5][0-9]", value) for value in labels)):
        return False
    buttons = [node for node in nodes if label(node) == "挂断" and
               node.get("clickable") == "true" and node.get("enabled") == "true"]
    if len(buttons) != 1:
        return False
    bounds = re.fullmatch(r"\[([0-9]+),([0-9]+)\]\[([0-9]+),([0-9]+)\]", buttons[0].get("bounds", ""))
    if not bounds:
        return False
    left, top, right, bottom = map(int, bounds.groups())
    x, y = point
    return left <= x < right and top <= y < bottom


def coordinate(values, length):
    if not isinstance(values, (list, tuple)) or len(values) != length or any(type(n) is not int for n in values):
        raise ValueError("invalid image coordinates")
    return tuple(values)


def _contains(outer, inner):
    left, top, right, bottom = inner
    return outer[0] <= left < right <= outer[2] and outer[1] <= top < bottom <= outer[3]


def _overlaps(a, b):
    return max(a[0], b[0]) < min(a[2], b[2]) and max(a[1], b[1]) < min(a[3], b[3])


class _SelectedPhone:
    def __init__(self, profile, test, view):
        try:
            from PIL import Image
        except ImportError as error:
            raise ConsoleError(f"verified {test} requires Pillow on the bench host") from error
        self.image = Image
        self.view = view
        self.serial = profile["adb_serial"]
        if not isinstance(self.serial, str) or not re.fullmatch(r"[A-Za-z0-9_.:-]{1,100}", self.serial):
            raise ValueError

    def crops(self, image):
        rgb = image.convert("RGB")
        return tuple(rgb.crop(box).tobytes() for box in self.boxes)

    def matches(self, image):
        return image.size == self.size and self.crops(image) == self.reference

    def adb(self, *args, timeout):
        try:
            result = subprocess.run(["adb", "-s", self.serial, *args], capture_output=True, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise ConsoleError("selected phone did not respond") from error
        if result.returncode:
            raise ConsoleError("selected phone rejected the bench operation")
        return result.stdout

    def frame(self, timeout=3):
        try:
            with self.image.open(io.BytesIO(self.adb("exec-out", "screencap", "-p", timeout=timeout)),
                                 formats=["PNG"]) as image:
                if image.size != self.size:
                    raise ConsoleError(f"selected phone screen does not match the reviewed {self.view} resolution")
                return image.convert("RGB")
        except OSError as error:
            raise ConsoleError("selected phone returned an unreadable screen") from error

    def tap(self, point):
        self.adb("shell", "input", "tap", *map(str, point), timeout=3)


class VerifiedPhone(_SelectedPhone):
    def __init__(self, profile, reference, base):
        try:
            super().__init__(profile, "wx-out", "banner")
            self.caller = reference["caller"]
            if not isinstance(self.caller, str) or not (1 <= len(self.caller.strip()) <= 64):
                raise ValueError
            self.caller = self.caller.strip()
            self.answer_point = coordinate(profile["answer_banner"], 2)
            self.boxes = [coordinate(profile[key], 4) for key in ("wx_caller_box", "wx_controls_box")]
            with self.image.open(Path(base) / reference["banner_png"], formats=["PNG"]) as image:
                self.size = image.size
                for x1, y1, x2, y2 in self.boxes:
                    if not (0 <= x1 < x2 <= image.width and 0 <= y1 < y2 <= image.height):
                        raise ValueError
                a, b, c, d = self.boxes[1]
                x, y = self.answer_point
                if not (a <= x < c and b <= y < d):
                    raise ValueError
                left, top, right, bottom = self.boxes[0]
                if not (right <= a or c <= left or bottom <= b or d <= top):
                    raise ValueError
                answer = image.convert("RGB").getpixel(self.answer_point)
                if any(abs(value - wanted) > 40 for value, wanted in zip(answer, (88, 190, 106))):
                    raise ValueError
                self.reference = self.crops(image)
        except (TypeError, ValueError, KeyError, OSError) as error:
            raise ConsoleError("configure a reviewed wx-out caller/banner and selected phone in devices.json") from error

    def answer(self, timeout=15, *, confirm=None):
        deadline = time.monotonic() + timeout
        while (remaining := deadline - time.monotonic()) > 0:
            if self.matches(self.frame(timeout=min(3, remaining))) and time.monotonic() < deadline:
                if confirm is not None:
                    confirm("PHONE_WX_OUT_ANSWER")
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not self.matches(self.frame(timeout=min(3, remaining))):
                        return False
                    if time.monotonic() >= deadline:
                        return False
                self.tap(self.answer_point)
                return True
            time.sleep(min(0.35, max(0, deadline - time.monotonic())))
        return False


def _same_pixels_once(image, box, template):
    """Reject repeated identity pixels, even outside the configured row."""
    left, top, right, bottom = box
    width, height = right - left, bottom - top
    stride, row = image.width * 3, width * 3
    pixels = image.tobytes()
    rows = [template[i:i + row] for i in range(0, len(template), row)]
    anchor = max(range(height), key=lambda i: len(set(rows[i])))
    position, candidates = -1, 0
    while (position := pixels.find(rows[anchor], position + 1)) != -1:
        candidates += 1
        if candidates > 64:
            return False
        y, x = divmod(position, stride)
        y -= anchor
        if x % 3 or x + row > stride or y < 0 or y + height > image.height:
            continue
        if (x, y) != (left * 3, top) and all(
                pixels[start:start + row] == expected
                for i, expected in enumerate(rows) for start in ((y + i) * stride + x,)):
            return False
    return True


class VerifiedIncomingPhone(_SelectedPhone):
    def __init__(self, profile, reference, base, expected_device):
        configuration = "wx-in unverified: configure a reviewed device card, account marker and selected phone"
        try:
            super().__init__(profile, "wx-in", "device list")
        except (TypeError, ValueError, KeyError) as error:
            raise ConsoleError(configuration) from error
        try:
            if (not isinstance(expected_device, str) or not expected_device.strip()
                    or reference["device_id"] != expected_device or reference["adb_serial"] != self.serial):
                raise ValueError
            self.boxes = [coordinate(reference[key], 4) for key in ("identity_box", "call_box", "account_box")]
            identity, call, account = self.boxes
            card = coordinate(reference["card_box"], 4)
            if (not _contains(card, identity) or not _contains(card, call)
                    or _overlaps(identity, call) or _overlaps(card, account)):
                raise ValueError
            with self.image.open(Path(base) / reference["list_png"], formats=["PNG"]) as image:
                self.size = image.size
                screen = (0, 0, image.width, image.height)
                if image.width * image.height > 8_500_000 or not _contains(screen, card):
                    raise ValueError
                for left, top, right, bottom in self.boxes:
                    if not (0 <= left < right <= image.width and 0 <= top < bottom <= image.height
                            and 6 <= right - left <= 768 and 6 <= bottom - top <= 192):
                        raise ValueError
                    colors = image.crop((left, top, right, bottom)).convert("RGB").getcolors(3)
                    if colors is not None and len(colors) < 3:
                        raise ValueError
                self.reference = self.crops(image)
            self.call_point = ((call[0] + call[2]) // 2, (call[1] + call[3]) // 2)
        except (TypeError, ValueError, KeyError, OSError, self.image.DecompressionBombError) as error:
            raise ConsoleError(configuration) from error

    def _verify_view(self, read_ui):
        activities = self.adb("shell", "dumpsys", "activity", "activities", timeout=3)
        resumed = [line for line in activities.splitlines() if b"topResumedActivity=" in line]
        if len(resumed) != 1 or not re.search(rb"\bcom\.tencent\.mm/\.plugin\.appbrand\.ui\.AppBrandUI[0-9]*\b", resumed[0]):
            raise ConsoleError("wx-in unverified: selected phone is not on the reviewed mini program")
        if b"displayId=" in resumed[0] and not re.search(rb"\bdisplayId=0\b", resumed[0]):
            raise ConsoleError("wx-in unverified: selected phone is on a different display")
        frame = self.frame()
        if not self.matches(frame) or not all(
                _same_pixels_once(frame, self.boxes[i], self.reference[i]) for i in (0, 2)):
            blocker = blocked_page(read_ui(self.serial))
            if blocker in (Page.LOGIN_EXPIRED, Page.EMPTY_LIST):
                raise ConsoleError(FAILURE[blocker])
            raise ConsoleError("wx-in unverified: reviewed device row, account or call button is absent or ambiguous")

    def dial(self, confirm, *, read_ui):
        self._verify_view(read_ui)
        confirm()
        self._verify_view(read_ui)
        self.tap(self.call_point)
