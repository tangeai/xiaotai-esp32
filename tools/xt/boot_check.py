"""Verify a fresh SDK boot after each explicitly authorized native USB reset."""

import ipaddress
import re
import time

import sdk_bench
from console_sdk import ConsoleError, Session, complete, identity


ACTION = "USB_CHIP_RESET"
USB = (0x303A, 0x1001)
CHIPS = frozenset(("esp32s3", "esp32c3"))
ROM = re.compile(r"ESP-ROM:(esp32[a-z0-9]*)-[A-Za-z0-9-]+\s*$")
RESET = re.compile(r"^rst:0x15 \(USB_UART_CHIP_RESET\),boot:0x[0-9a-f]+ \(SPI_FAST_FLASH_BOOT\)$")
CRASH = re.compile(r"Guru Meditation|Backtrace:|assert failed|abort\(\) was called|Brownout detector", re.I)
TOLERANCE = 0.250
COMMANDS = ("AT+STATE", "AT+NET", "AT+PLATFORM")


def bind_usb(ports, port, serial_number):
    def name(item):
        return item.casefold() if item.upper().startswith("COM") else item

    ports = list(ports)
    matches = [item for item in ports if name(item.device) == name(port)]
    native = [item for item in ports if (item.vid, item.pid) == USB and item.serial_number and
              item.serial_number.casefold() == serial_number.casefold()]
    if len(matches) != 1 or len(native) != 1 or matches[0] is not native[0]:
        raise ConsoleError("port does not match one confirmed native USB Serial/JTAG device")
    return matches[0].device


def chip_reset(port, reset, confirm, clock=time.monotonic):
    # Exclude both an in-progress SDK transaction and its async reader while
    # discarding prior bytes and taking the start mark for this reset only.
    with port.sdk_lock:
        port.rx_resume.clear()
        try:
            with port.rx_lock:
                confirm()
                port.serial.reset_input_buffer()
                port.partial = b""
                mark, started = port.mark(), clock()
                reset(port.serial, uses_usb=True).reset()
        finally:
            port.rx_resume.set()
    return mark, started


def validate_request(allow, serial_number, expected_sha, app, rounds, timeout):
    if allow != [ACTION]:
        raise ConsoleError(f"boot-check requires exactly --allow {ACTION}")
    if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", serial_number):
        raise ConsoleError("boot-check requires the confirmed USB serial number")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", expected_sha):
        raise ConsoleError("expected a full 64-digit ELF SHA-256")
    if not re.fullmatch(r"[a-z][a-z0-9_.-]{0,47}", app):
        raise ConsoleError("boot-check requires the expected foreground app")
    if not 1 <= rounds <= 10 or not 10 <= timeout <= 120:
        raise ConsoleError("boot-check requires 1..10 rounds and a 10..120 second timeout")


def connect(port, vocabulary, expected_sha, detected=None, *, expected_chip=None):
    if not vocabulary.board:
        raise ConsoleError("boot-check requires a vocabulary with exact board identity")
    session = Session(port.sdk_exchange, vocabulary, (), expected_sha, detected=detected)
    chip = session.device["chip"]
    if expected_chip is not None and chip != expected_chip:
        raise ConsoleError("running chip changed after USB reset")
    if chip not in CHIPS:
        raise ConsoleError("native USB reset requires an identified ESP32-S3 or ESP32-C3 SDK build")
    if any(session.check(name).access != "read" for name in COMMANDS):
        raise ConsoleError("boot evidence must use read-only SDK commands")
    return session


def state(session, app, clock):
    first = clock()
    row = sdk_bench.state(session)
    last = clock()
    uptime = sdk_bench.unsigned(row, "uptime_ms")
    boot = (first - uptime / 1000, last - uptime / 1000)
    ready = all(row.get(key) == value for key, value in
                (("started", "1"), ("app", app), ("activity", "none"), ("id", "0")))
    return uptime, boot, ready


def observe_rom(port, mark, chip):
    lines = port.since(mark)
    banners = [(index, banner[1]) for index, line in enumerate(lines) if (banner := ROM.search(line))]
    reasons = [index for index, line in enumerate(lines) if RESET.fullmatch(line)]
    if any(found != chip for _, found in banners):
        raise ConsoleError("ROM chip does not match the identified firmware")
    if (any(CRASH.search(line) for line in lines) or len(banners) > 1 or len(reasons) > 1 or
            any(line.startswith("rst:") and not RESET.fullmatch(line) for line in lines)):
        raise ConsoleError("unexpected second boot, reset cause or crash")
    return len(banners) == len(reasons) == 1 and banners[0][0] < reasons[0]


def network(session):
    row = sdk_bench.one(session, "AT+NET", "+NET: ")
    try:
        address = ipaddress.ip_address(row.get("ip", "-"))
        usable = not (address.is_unspecified or address.is_loopback or address.is_multicast or
                      address.is_link_local or address.is_reserved)
    except ValueError:
        usable = False
    return usable and all(row.get(key) == value for key, value in
                          (("state", "up"), ("connected", "1"), ("provisioning", "0"), ("held", "0")))


def platform(session):
    row = sdk_bench.one(session, "AT+PLATFORM", "+PLATFORM: ")
    return all(row.get(key) == value for key, value in (
        ("online", "1"), ("clock", "1"), ("discovered", "1"), ("authorized", "1"),
        ("signaling", "1"), ("verifying", "0"), ("held", "0"), ("gated", "0")))


def run(open_port, reset_port, confirm, vocabulary, expected_sha, app, rounds, timeout, *,
        clock=time.monotonic, sleep=time.sleep):
    """Use at most one reset per round and stop at the first unproven layer."""
    report = {"schema": 1, "reset_kind": "native_usb_chip_reset", "cold_boot": False,
              "board": vocabulary.board, "expected_elf_sha256": expected_sha.lower(),
              "expected_app": app, "requested_rounds": rounds, "pass": False, "rounds": []}
    for index in range(1, rounds + 1):
        row = {"round": index, "attempted": False, "rom_reset": False, "boot": False,
               "app": False, "network": False, "platform": False}
        report["rounds"].append(row)
        stage = "preflight"
        try:
            with open_port() as port:
                pre = connect(port, vocabulary, expected_sha)
                chip = pre.device["chip"]
                row["pre_uptime_ms"], prior, idle = state(pre, app, clock)
                remaining = 1.0 - (clock() - prior[1])
                if idle and remaining > 0:
                    sleep(remaining)
                    row["pre_uptime_ms"], prior, idle = state(pre, app, clock)
                if not idle or clock() - prior[1] < 1.0:
                    raise ConsoleError("expected app must be idle with a fresh measurable uptime baseline")
                stage = "reset"
                row["attempted"] = True
                mark, started = reset_port(port)
                deadline = started + timeout
                stage = "rom"
                while clock() < deadline and not observe_rom(port, mark, chip):
                    sleep(0.1)
                if clock() >= deadline:
                    raise ConsoleError("no fresh native USB chip reset boot record")
                row["rom_reset"] = True
                stage = "identity"
                post = None
                while clock() < deadline:
                    confirm()
                    observe_rom(port, mark, chip)
                    try:
                        lines = port.sdk_exchange("AT+VERSION", min(2, deadline - clock()))
                    except ConsoleError:
                        sleep(0.1)
                        continue
                    if lines[-1:] == ["ERROR: INVALID_STATE STARTING"]:
                        sleep(0.1)
                        continue
                    found = identity(complete(lines, "post-reset identity"))
                    if not found:
                        raise ConsoleError("no identified SDK firmware after reset")
                    post = connect(port, vocabulary, expected_sha, detected=found, expected_chip=chip)
                    break
                if post is None:
                    raise ConsoleError("no verified SDK identity after reset")
                stage = "uptime"
                row["post_uptime_ms"], boot, row["app"] = state(post, app, clock)
                if boot[0] < started - TOLERANCE or boot[0] <= prior[1] + TOLERANCE:
                    raise ConsoleError("uptime does not establish a new boot after this reset")
                row["boot"] = True
                stage = "ready"
                while clock() < deadline:
                    confirm()
                    observe_rom(port, mark, chip)
                    row["network"] = network(post)
                    row["platform"] = platform(post)
                    row["final_uptime_ms"], now, row["app"] = state(post, app, clock)
                    if (now[0] > boot[1] + TOLERANCE or now[1] < boot[0] - TOLERANCE or
                            row["final_uptime_ms"] < row["post_uptime_ms"]):
                        raise ConsoleError("app and online samples span different boots")
                    if row["app"] and row["network"] and row["platform"] and clock() < deadline:
                        observe_rom(port, mark, chip)
                        confirm()
                        break
                    sleep(0.25)
                else:
                    raise ConsoleError("application, network or platform not ready before timeout")
        except ConsoleError as error:
            row.update(stop_stage=stage, error=str(error))
            return report
        except (OSError, RuntimeError):
            row.update(stop_stage=stage, error="USB reset or serial transport failed")
            return report
    report["pass"] = True
    return report
