"""Conservative automatic UART baud selection for the local flash command."""

import hashlib
import os
from pathlib import Path
import re
import stat

BRIDGE_BAUD = 921600
LEGACY_BAUD = 460800
FALLBACK_BAUD = 115200
# Confirmed on the lab's Waveshare and WT99 CH343 ports. No inference from
# driver descriptions, tty naming, chip type or related vendor product IDs.
USB_UART = {(0x1A86, 0x55D3)}
RATE_ERROR = "A fatal error occurred: Failed to set baud rate 921600. The driver may not support this rate."
DISALLOWED = re.compile(
    r"erase|writing at|written|wrote|compressed|verify|verification|\bhash\b|\bmd5\b|encrypted|secure boot|"
    r"wrong chip|not an? esp|device disconnected|permission denied|invalid (?:argument|image|offset)|"
    r"cancelled|canceled|keyboardinterrupt|aborted", re.I)


def same_port(a, b):
    if re.fullmatch(r"COM[0-9]+", a, re.I) and re.fullmatch(r"COM[0-9]+", b, re.I):
        return a.casefold() == b.casefold()
    return a == b


def hw_id(item):
    return item.vid, item.pid


def usb_bridge(list_ports, port):
    """Return (confirmed bridge, descriptor serial, strong retry identity)."""
    if list_ports is None:
        return False, None, None
    try:
        ports = list(list_ports.comports())
    except OSError:
        return False, None, None
    matches = [item for item in ports if same_port(item.device, port)]
    if len(matches) != 1 or hw_id(matches[0]) not in USB_UART:
        return False, None, None
    item = matches[0]
    serial = item.serial_number
    location = item.location
    if (not serial or not re.fullmatch(r"[A-Za-z0-9._:-]{4,128}", serial) or not location or
            location.casefold() in {"unknown", "none", "n/a"} or
            serial.casefold() in {"unknown", "none", "serial", "123456", "12345678"} or set(serial) == {"0"}):
        return True, serial, None
    peers = [other for other in ports if hw_id(other) == hw_id(item) and other.serial_number and
             other.serial_number.casefold() == serial.casefold()]
    return True, serial, (*hw_id(item), serial.casefold(), location) if len(peers) == 1 else None


def file_identity(st, *, windows=os.name == "nt"):
    # On Windows, stat(path) and fstat(handle) can disagree on st_ctime_ns
    # for the same file. Windows exposes creation time independently.
    extra = getattr(st, "st_birthtime_ns", None) if windows else st.st_ctime_ns
    return st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, extra


def image_fingerprint(filename):
    try:
        path = Path(filename).resolve(strict=True)
        initial = path.stat()
        if not stat.S_ISREG(initial.st_mode) or not initial.st_ino:
            return None
        with path.open("rb") as stream:
            expected = file_identity(initial)
            if file_identity(os.fstat(stream.fileno())) != expected:
                return None
            digest = hashlib.sha256()
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
            if file_identity(os.fstat(stream.fileno())) != expected or file_identity(path.stat()) != expected:
                return None
        return path, expected, digest.digest()
    except (OSError, RuntimeError, ValueError):
        return None


def prewrite_rate_failure(result):
    """Recognize only the pinned esptool 4/5 host baud-setter error."""
    if result.returncode != 2:
        return False
    lines = [line.strip() for line in (result.stdout + "\n" + result.stderr).splitlines()]
    fatal = [line for line in lines if line.startswith("A fatal error occurred:")]
    return (fatal == [RATE_ERROR] and
            any(line in {"Changing baud rate to 921600", "Changing baud rate to 921600..."} for line in lines) and
            not any(DISALLOWED.search(line) for line in lines))
