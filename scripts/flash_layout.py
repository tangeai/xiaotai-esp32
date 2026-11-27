"""Record and verify the IDF-generated flash layout without touching a device."""
import hashlib
import json
from pathlib import PurePosixPath
import re


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate key in flasher_args.json: {key}")
        result[key] = value
    return result


def _offset(value):
    if not isinstance(value, str) or not re.fullmatch(r"0x[0-9a-fA-F]+", value):
        raise ValueError(f"Invalid flash offset: {value!r}")
    offset = int(value, 16)
    if offset % 0x1000:
        raise ValueError(f"Flash offset is not sector aligned: {value}")
    return offset


def _file_in_build(folder, name):
    if not isinstance(name, str) or not name or "\\" in name or ":" in name:
        raise ValueError(f"Invalid flash file: {name!r}")
    path = PurePosixPath(name)
    actual = (folder / path).resolve()
    if path.is_absolute() or ".." in path.parts or path.as_posix() != name or not actual.is_relative_to(folder):
        raise ValueError(f"Flash file outside build directory: {name}")
    if not actual.is_file() or not actual.stat().st_size:
        raise ValueError(f"Missing or empty flash file: {name}")
    return actual


def flash_layout(folder, target, *, app_name="xiaotai.bin"):
    folder = folder.resolve()
    raw = _file_in_build(folder, "flasher_args.json").read_bytes()
    try:
        args = json.loads(raw, object_pairs_hook=_unique_object)
        chip = args["extra_esptool_args"]["chip"]
        size = args["flash_settings"]["flash_size"]
        files = args["flash_files"]
        sections = {key: args[key] for key in ("app", "bootloader", "partition-table")}
    except (KeyError, TypeError) as error:
        raise ValueError("Incomplete flasher_args.json") from error
    if chip != target:
        raise ValueError(f"Flash target differs: expected {target}, got {chip}")
    capacity = re.fullmatch(r"([1-9][0-9]*)MB", size) if isinstance(size, str) else None
    if not capacity or not isinstance(files, dict) or not files:
        raise ValueError("Invalid flash size or segment list")
    limit = int(capacity[1]) * 1024 * 1024
    segments = []
    end = 0
    ordered = sorted(((_offset(key), name) for key, name in files.items()), key=lambda pair: pair[0])
    for offset, name in ordered:
        if offset < end:
            raise ValueError(f"Overlapping flash segment at {offset:#x}")
        path = _file_in_build(folder, name)
        length = path.stat().st_size
        end = offset + length
        if end > limit:
            raise ValueError(f"Flash segment exceeds {size}: {name}")
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        segments.append({"offset": hex(offset), "file": name, "bytes": length, "sha256": digest})
    by_offset = {row["offset"]: row["file"] for row in segments}
    for key, section in sections.items():
        if not isinstance(section, dict) or "offset" not in section or not isinstance(section.get("file"), str):
            raise ValueError(f"Invalid {key} flash section")
        if by_offset.get(hex(_offset(section["offset"]))) != section["file"]:
            raise ValueError(f"{key} does not match flash_files")
    if sections["app"]["file"] != app_name:
        raise ValueError(f"Flash app must be {app_name}")
    return {"chip": chip, "flash_size": size, "app_offset": hex(_offset(sections["app"]["offset"])),
            "flasher_args_sha256": hashlib.sha256(raw).hexdigest(), "segments": segments}
