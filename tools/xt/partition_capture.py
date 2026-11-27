"""One explicitly authorized local ROM connection for exactly one partition table."""

from contextlib import closing, redirect_stdout
from dataclasses import dataclass
import re
import sys

from partition_plan import (FLASH_SIZES, IMAGE_CHIP_IDS, TABLE_BYTES, TABLE_OFFSET,
                            assign_built_slot, checked_build, plan_bytes)


ACTION = "PARTITION_TABLE_ROM_RESET"
ROM_VERSION = "5.3.1"
PORT_PATTERN = r"COM[1-9][0-9]*|/dev/(?:ttyUSB|ttyACM)[0-9]+|/dev/cu\.[A-Za-z0-9_-]+"
USB_PATTERN = r"[0-9a-fA-F]{4}:[0-9a-fA-F]{4}"
USB_SERIAL_PATTERN = r"[A-Za-z0-9_.:-]{1,128}"
MAC_PATTERN = r"[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}"
REV_PATTERN = r"(0|[1-9][0-9]?)\.(0|[1-9][0-9]?)"
BOOT_USB = {(0x303A, 0x1001), (0x1A86, 0x55D3)}


@dataclass(frozen=True)
class Target:
    port: str
    board: str
    chip: str
    usb: tuple
    usb_serial: str
    mac: tuple
    revision: tuple


def request(allow, port, board, chip, usb, usb_serial, mac, revision):
    if allow != [ACTION]:
        raise ValueError(f"live partition read requires exactly --allow {ACTION}")
    if not port or not re.fullmatch(PORT_PATTERN, port):
        raise ValueError("live partition read requires an explicit local USB serial port")
    if not board or chip not in IMAGE_CHIP_IDS:
        raise ValueError("confirm the physical board model and expected ESP chip")
    if not usb or not re.fullmatch(USB_PATTERN, usb) or not usb_serial or not re.fullmatch(USB_SERIAL_PATTERN, usb_serial):
        raise ValueError("confirm the USB VID:PID and its nonempty descriptor serial number")
    vid_pid = tuple(int(part, 16) for part in usb.split(":"))
    if vid_pid not in BOOT_USB:
        raise ValueError("USB interface outside the reviewed native USB and CH343 set")
    if not mac or not re.fullmatch(MAC_PATTERN, mac):
        raise ValueError("confirm the MCU ROM base MAC independently of any USB bridge")
    base_mac = tuple(int(part, 16) for part in mac.split(":"))
    if not any(base_mac) or base_mac[0] & 1:
        raise ValueError("expected a nonzero unicast MCU ROM base MAC")
    if not revision or not re.fullmatch(REV_PATTERN, revision):
        raise ValueError("confirm the ROM chip revision as major.minor")
    revision_pair = tuple(int(part) for part in revision.split("."))
    if chip == "esp32p4" and revision_pair[0] != 3:
        raise ValueError("only rev3.x ESP32-P4 is supported")
    return Target(port, board, chip, vid_pid, usb_serial, base_mac, revision_pair)


def bind_usb(ports, target):
    def key(name):
        return name.upper() if name.upper().startswith("COM") else name

    ports = tuple(ports)
    candidates = [p for p in ports if (p.vid, p.pid) == target.usb and p.serial_number and
                  p.serial_number.casefold() == target.usb_serial.casefold()]
    explicit = [p for p in ports if key(p.device) == key(target.port)]
    if len(candidates) != 1 or len(explicit) != 1 or explicit[0] is not candidates[0]:
        raise ValueError("explicit port does not match one confirmed USB descriptor; no port switching")
    return explicit[0].device


def esptool_adapter(chip):
    try:
        import esptool
        from esptool import cmds, loader
        from esptool.targets import CHIP_DEFS
    except ImportError as error:
        raise ValueError(f"live partition read requires esptool {ROM_VERSION} in this Python") from error
    if esptool.__version__ != ROM_VERSION:
        raise ValueError(f"live partition read is reviewed only against esptool {ROM_VERSION}")
    if any(loader.cfg.get(name) is not None for name in ("custom_reset_sequence", "custom_hard_reset_sequence")):
        raise ValueError("live partition read does not allow custom esptool reset sequences")
    return CHIP_DEFS[chip], cmds.detect_flash_size


def capture(target, size, *, list_usb, serial_factory, rom_factory, flash_size):
    """Use the single attested ROM. Local read count and address are fixed by the tool."""
    def confirm():
        return bind_usb(list_usb(), target)

    port = confirm()
    identity_ok = reset_attempted = False
    failure = restore_failure = None
    stage = "serial open"
    with closing(serial_factory(port=None, baudrate=115200, exclusive=True, write_timeout=2)) as wire:
        try:
            wire.port, wire.dtr, wire.rts = port, False, False
            wire.open()
            confirm()
            rom = rom_factory(wire, 115200)
            stage = "ROM connection"
            reset_attempted = True
            rom.connect(mode="default-reset", attempts=1, warnings=False)
            stage = "ROM hardware identity"
            info = rom.get_security_info()
            if (rom.IS_STUB or rom.sync_stub_detected or info.get("chip_id") != IMAGE_CHIP_IDS[target.chip]):
                raise ValueError("unexpected chip ID or non-ROM connection")
            flags = info.get("parsed_flags", {})
            if flags.get("SECURE_DOWNLOAD_ENABLE") is not False or rom.secure_download_mode:
                raise ValueError("secure or unverified ROM download mode")
            if tuple(rom.read_mac()) != target.mac:
                raise ValueError("ROM MCU identity does not match the confirmed MAC")
            confirm()
            identity_ok = True
            actual_revision = (rom.get_major_chip_version(), rom.get_minor_chip_version())
            if actual_revision != target.revision:
                raise ValueError("ROM revision does not match the confirmed hardware")
            stage = "security policy"
            crypt = info.get("flash_crypt_cnt")
            if (flags.get("SECURE_BOOT_EN") is not False or type(crypt) is not int or not 0 <= crypt <= 255
                    or crypt.bit_count() % 2):
                raise ValueError("secure boot or flash encryption is enabled or unknown")
            stage = "SPI NOR attachment and capacity"
            rom.flash_spi_attach(0)
            if flash_size(rom) != size:
                raise ValueError("physical flash capacity differs from verified build or is unknown")
            rom.flash_set_parameters(FLASH_SIZES[size] * 1024 * 1024)
            stage = "fixed partition-table read"
            raw = rom.read_flash(TABLE_OFFSET, TABLE_BYTES)
            if not isinstance(raw, bytes) or len(raw) != TABLE_BYTES:
                raise ValueError("unexpected partition read length")
        except Exception as error:
            failure = ValueError(f"{stage}: {error}")
        finally:
            if identity_ok:
                try:
                    confirm()
                    if not wire.is_open:
                        raise ValueError("original port closed")
                    rom.hard_reset()
                except Exception as error:
                    restore_failure = error
    if restore_failure is not None:
        raise ValueError(f"{failure or 'partition read completed'}, but hard reset failed; check the board manually") from restore_failure
    if failure is not None:
        suffix = "; verify device boot manually before retry" if reset_attempted and not identity_ok else ""
        raise ValueError(f"{failure}{suffix}") from failure
    return raw, {"source": "live-rom", "read_offset": hex(TABLE_OFFSET), "read_bytes": TABLE_BYTES,
                 "board_model_operator_confirmed": target.board, "chip": target.chip,
                 "revision": f"{actual_revision[0]}.{actual_revision[1]}", "flash_size": size,
                 "usb_vid_pid": f"{target.usb[0]:04x}:{target.usb[1]:04x}", "usb_and_rom_identity_match": True,
                 "rom_security": {"secure_download": False, "secure_boot": False, "flash_encryption": False},
                 "download_rom_reset": True, "hard_reset_requested": True, "app_boot_verified": False}


def live_plan(folder, target, *, list_usb, serial_factory, python):
    folder = folder.resolve()
    build = checked_build(folder)
    chip, board, settings, *_ = build
    if (chip, board) != (target.chip, target.board):
        raise ValueError("confirmed board or chip differs from the verified build")
    assign_built_slot(build)
    # Keep esptool progress and warning text out of the machine-readable plan.
    with redirect_stdout(sys.stderr):
        rom_factory, flash_size = esptool_adapter(chip)
        raw, source = capture(target, settings["flash_size"], list_usb=list_usb, serial_factory=serial_factory,
                              rom_factory=rom_factory, flash_size=flash_size)
    if checked_build(folder) != build:
        raise ValueError("verified build changed during the ROM read; do not use its old preview")
    report = plan_bytes(folder, build, raw, port=target.port, python=python)
    report["capture"] = source
    report["unverified"] = ["physical board model and running old app are not independently identified by ROM",
                            "installed bootloader, boot policy and other eFuse settings",
                            "application reboot/health after leaving ROM",
                            "NVS contents and format"]
    return report
