"""Assess an ESP partition update using an exact table and a verified build."""

from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import re
import struct

from flash_layout import flash_layout


TABLE_OFFSET = 0x8000
TABLE_BYTES = 0xC00
SECTOR = 0x1000
ENTRY = struct.Struct("<HBBII16sI")
ERASED = b"\xff" * ENTRY.size
MD5_TAG = b"\xeb\xeb" + b"\xff" * 14
# These are the plain single-factory IDF targets currently used by XiaoTai.
BOOT_OFFSETS = {"esp32c3": 0, "esp32s3": 0, "esp32p4": 0x2000}
IMAGE_CHIP_IDS = {"esp32c3": 0x05, "esp32s3": 0x09, "esp32p4": 0x12}
FLASH_SIZES = {"2MB": 2, "4MB": 4, "8MB": 8, "16MB": 16, "32MB": 32}
ROLES = ("bootloader", "partition-table", "app")


@dataclass(frozen=True)
class Partition:
    label: str
    kind: int
    subtype: int
    offset: int
    size: int
    flags: int

    @property
    def end(self):
        return self.offset + self.size


def parse_table(raw, limit, source):
    """Require a single correct MD5, an end marker and an erased remainder."""
    def fail(reason):
        raise ValueError(f"{source}: {reason}")

    if len(raw) != TABLE_BYTES:
        fail(f"expected exactly {TABLE_BYTES} bytes from {TABLE_OFFSET:#x}")
    rows = []
    seen = set()
    for at in range(0, TABLE_BYTES, ENTRY.size):
        block = raw[at:at + ENTRY.size]
        if block[:16] == MD5_TAG:
            if not rows or block[16:] != hashlib.md5(raw[:at], usedforsecurity=False).digest():
                fail("wrong partition MD5")
            tail = raw[at + ENTRY.size:]
            if not tail or tail != b"\xff" * len(tail):
                fail("missing terminator or data after the MD5")
            break
        if block == ERASED:
            fail("missing partition MD5")
        magic, kind, subtype, offset, size, label, flags = ENTRY.unpack(block)
        if magic != 0x50AA:
            fail(f"unknown partition record at {at:#x}")
        name = label.rstrip(b"\0")
        if not re.fullmatch(rb"[A-Za-z0-9_.-]{1,16}", name) or name in seen:
            fail(f"invalid or duplicate label at {at:#x}")
        seen.add(name)
        align = 0x10000 if kind == 0 else SECTOR
        if (offset < TABLE_OFFSET + SECTOR or offset % align or not size or size % SECTOR
                or offset + size > limit):
            fail(f"invalid range for {name.decode()}")
        rows.append(Partition(name.decode(), kind, subtype, offset, size, flags))
    else:
        fail("partition table is not terminated")
    ordered = sorted(rows, key=lambda row: row.offset)
    if any(left.end > right.offset for left, right in zip(ordered, ordered[1:])):
        fail("overlapping partitions")
    return rows


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate manifest key: {key}")
        result[key] = value
    return result


def small_file(path, maximum):
    if not path.is_file():
        raise ValueError(f"missing regular input file: {path}")
    with path.open("rb") as stream:
        data = stream.read(maximum + 1)
    if len(data) > maximum:
        raise ValueError(f"input exceeds {maximum} bytes: {path}")
    return data


def checked_build(folder):
    if any((folder / name).resolve().parent != folder for name in ("flasher_args.json", "build-record.json")):
        raise ValueError("manifest outside build directory")
    args_raw = small_file(folder / "flasher_args.json", 64 * 1024)
    record_raw = small_file(folder / "build-record.json", 1024 * 1024)
    try:
        args = json.loads(args_raw, object_pairs_hook=unique_object)
        record = json.loads(record_raw, object_pairs_hook=unique_object)
        chip = record["target"]
        settings = args["flash_settings"]
        size = settings["flash_size"]
        mode = settings["flash_mode"]
        freq = settings["flash_freq"]
        roles = {role: args[role] for role in ROLES}
        app_name = roles["app"]["file"]
    except (KeyError, TypeError) as error:
        raise ValueError("incomplete firmware flash manifest") from error
    if (record.get("stage") != "firmware" or not isinstance(record.get("board"), str) or not record["board"]
            or not isinstance(chip, str) or chip not in BOOT_OFFSETS
            or not isinstance(size, str) or size not in FLASH_SIZES
            or mode not in ("dio", "dout", "qio", "qout") or freq not in ("20m", "26m", "40m", "80m", "120m")):
        raise ValueError("unsupported firmware, chip or flash settings")
    if any(not isinstance(section, dict) or
           not (section.get("encrypted") is False or section.get("encrypted") == "false")
           for section in roles.values()):
        raise ValueError("encrypted or unspecified flash write is not supported")
    layout = flash_layout(folder, chip, app_name=app_name)
    if (layout != record.get("flash_layout")
            or layout["flasher_args_sha256"] != hashlib.sha256(args_raw).hexdigest()):
        raise ValueError("flash manifest or image bytes do not match the verified build record")
    if (len(layout["segments"]) != len(ROLES)
            or len({section["file"] for section in roles.values()}) != len(ROLES)):
        raise ValueError("only a dedicated bootloader, table and application are supported")
    by_file = {row["file"]: row for row in layout["segments"]}
    pieces = {role: by_file[section["file"]] for role, section in roles.items()}
    boot, table, app = (pieces[role] for role in ROLES)
    artifacts = record.get("artifacts")
    if not isinstance(artifacts, dict) or artifacts.get(app_name) != app["sha256"]:
        raise ValueError("application digest is missing or differs in the firmware artifacts record")
    if (int(boot["offset"], 16) != BOOT_OFFSETS[chip] or int(table["offset"], 16) != TABLE_OFFSET
            or int(boot["offset"], 16) + boot["bytes"] > TABLE_OFFSET):
        raise ValueError("unsupported bootloader or partition table address")
    if table["bytes"] != TABLE_BYTES:
        raise ValueError("built table is not the expected 3072-byte IDF image")
    raw = small_file(folder / table["file"], TABLE_BYTES)
    if hashlib.sha256(raw).hexdigest() != table["sha256"]:
        raise ValueError("built table changed during verification")
    for role, image in (("bootloader", boot), ("app", app)):
        with (folder / image["file"]).open("rb") as stream:
            header = stream.read(36)
        if (len(header) != 36 or header[0] != 0xE9 or
                struct.unpack_from("<H", header, 12)[0] != IMAGE_CHIP_IDS[chip] or
                (role == "app" and struct.unpack_from("<I", header, 32)[0] != 0xABCD5432)):
            raise ValueError(f"{role} is not a {chip} {role} image")
    return chip, record["board"], settings, pieces, raw, hashlib.sha256(record_raw).hexdigest()


def factory(rows):
    apps = [row for row in rows if row.kind == 0]
    if (len(apps) != 1 or apps[0].subtype != 0
            or any(row.kind not in (0, 1) or row.flags or (row.kind == 1 and row.subtype == 0) for row in rows)):
        raise ValueError("only unflagged single-factory layouts without OTA are supported")
    return apps[0]


def selected(rows, kind, subtype):
    return {row for row in rows if row.kind == kind and row.subtype == subtype}


def erase_end(segment):
    end = int(segment["offset"], 16) + segment["bytes"]
    return (end + SECTOR - 1) // SECTOR * SECTOR


def assign_built_slot(build):
    _, _, settings, pieces, built_raw, _ = build
    limit = FLASH_SIZES[settings["flash_size"]] * (1024 * 1024)
    new = parse_table(built_raw, limit, "built table")
    slot = factory(new)
    app = pieces["app"]
    if int(app["offset"], 16) != slot.offset or erase_end(app) > slot.end:
        raise ValueError("application write does not fit the built factory partition")
    return limit, new


def plan_bytes(folder, build, saved_raw, *, port=None, python="python"):
    chip, board, settings, pieces, built_raw, record_sha = build
    limit, new = assign_built_slot(build)
    old = parse_table(saved_raw, limit, "saved table")
    factory(old)
    app = pieces["app"]
    # Do not assume that matching addresses prove preferences format compatibility.
    nvs_old = selected(old, 1, 2)
    nvs_unchanged = any(row.label == "nvs" for row in nvs_old) and nvs_old == selected(new, 1, 2)
    phy_unchanged = selected(old, 1, 1) == selected(new, 1, 1)
    data_unchanged = {row for row in old if row.kind == 1} == {row for row in new if row.kind == 1}
    if not (nvs_unchanged and phy_unchanged and data_unchanged):
        mode, roles = "manual-full-review", ()
    elif saved_raw == built_raw:
        mode, roles = "app-only", ("app",)
    else:
        mode, roles = "three-segment", ROLES
    argv = None
    if port is not None:
        if not re.fullmatch(r"COM[1-9][0-9]*|/dev/[A-Za-z0-9_.-]+", port):
            raise ValueError("use an explicit COM or /dev port for an esptool preview")
        if roles:
            argv = [python, "-m", "esptool", "--chip", chip, "--port", port, "write-flash"]
            if mode == "three-segment":
                for key in ("flash_mode", "flash_size", "flash_freq"):
                    argv.extend(("--" + key.replace("_", "-"), settings[key]))
            for role in roles:
                row = pieces[role]
                argv.extend((row["offset"], str(folder / row["file"])))
    old_by_name = {row.label: row for row in old}
    new_by_name = {row.label: row for row in new}
    return {
        "schema": 1, "mode": mode, "write_authorized": False, "esptool_preview_argv": argv,
        "build_board": board, "chip": chip, "flash_size": settings["flash_size"], "table_offset": hex(TABLE_OFFSET),
        "build_record_sha256": record_sha, "saved_table_sha256": hashlib.sha256(saved_raw).hexdigest(),
        "built_table_sha256": pieces["partition-table"]["sha256"], "app": app,
        "nvs_layout_unchanged": nvs_unchanged, "phy_layout_unchanged": phy_unchanged,
        "data_layout_unchanged": data_unchanged,
        "preview_segments": [{"role": role, **pieces[role], "erase_end_exclusive": hex(erase_end(pieces[role]))}
                             for role in roles],
        "changes": [{"label": name,
                     "old": asdict(old_by_name[name]) if name in old_by_name else None,
                     "new": asdict(new_by_name[name]) if name in new_by_name else None}
                    for name in sorted(old_by_name.keys() | new_by_name.keys())
                    if old_by_name.get(name) != new_by_name.get(name)],
        "unverified": ["device identity, chip revision and flash capacity", "capture address and board association",
                       "installed bootloader and boot policy",
                       "eFuse, flash encryption and secure boot", "NVS contents and format"],
    }


def plan(folder, board_table, *, port=None, python="python"):
    folder = folder.resolve()
    build = checked_build(folder)
    return plan_bytes(folder, build, small_file(board_table, TABLE_BYTES), port=port, python=python)
