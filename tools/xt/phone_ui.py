"""Read only the stable XiaoTai mini-program markers exposed by Android UI."""
from enum import Enum
from xml.etree import ElementTree as ET


class Page(Enum):
    READY = "ready"
    LOGIN_EXPIRED = "login_expired"
    EMPTY_LIST = "empty_list"
    UNKNOWN = "unknown"


FAILURE = {
    Page.LOGIN_EXPIRED: "XiaoTai mini-program login expired; sign in again",
    Page.EMPTY_LIST: "XiaoTai device list is empty; the target is not on this account's list",
    Page.UNKNOWN: "XiaoTai phone page is unrecognized; target presence could not be checked",
}
MAX_DUMP_BYTES = 1024 * 1024
AUTH_MARKERS = {
    "登录状态已过期",
    "登录状态已过期，请重新登录",
    "登录状态已过期，请重新登录。",
}


def wechat_nodes(dump):
    """Read a bounded UI snapshot and return only the WeChat nodes."""
    if not dump or len(dump) > MAX_DUMP_BYTES:
        return ()
    try:
        xml = dump.decode("utf-8", errors="strict")
        if "\0" in xml or "<!DOCTYPE" in xml.upper():
            return ()
        root = ET.fromstring(xml)
    except (UnicodeDecodeError, ET.ParseError):
        return ()
    if root.tag != "hierarchy":
        return ()
    return tuple(node for node in root.iter("node") if node.get("package") == "com.tencent.mm")


def blocked_page(dump):
    """Only recognize explicit blockers, not absent rows in a scrollable viewport."""
    labels = {" ".join(node.get(key, "").split())
              for node in wechat_nodes(dump)
              for key in ("text", "content-desc")}
    if "小钛" not in labels:
        return Page.UNKNOWN
    if labels & AUTH_MARKERS:
        return Page.LOGIN_EXPIRED
    if "添加你的第一台设备" in labels and "添加设备" in labels:
        return Page.EMPTY_LIST
    return Page.UNKNOWN
