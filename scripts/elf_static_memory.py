"""Record selected static memory sections from a verified ESP-IDF app ELF."""

import hashlib
import json
from pathlib import Path


SCHEMA = 1
THRESHOLD_BYTES = 1024
CORE_SECTIONS = (".iram0.text", ".dram0.data", ".dram0.bss")
OTHER_SECTIONS = {".iram0.vectors", ".iram0.data", ".iram0.bss", ".noinit",
                  ".rtc.text", ".rtc.data", ".rtc.bss", ".rtc.noinit",
                  ".rtc.force_fast", ".rtc.force_slow", ".rtc_reserved"}
PLACED_SECTIONS = {f".{region}.{section}" for region in ("ext_ram", "spm", "tcm")
                   for section in ("text", "rodata", "data", "bss", "noinit")}
XIP_SECTIONS = {".flash.text": "CONFIG_SPIRAM_FETCH_INSTRUCTIONS",
                ".flash.rodata": "CONFIG_SPIRAM_RODATA"}


def section_sizes(sections):
    # ALLOC alone is not enough: IRAM aliases and PSRAM mapping placeholders are ALLOC too.
    observed = {section.name: section["sh_size"] for section in sections
                if section["sh_flags"] & 2}  # ELF SHF_ALLOC
    if not any(name in observed for name in CORE_SECTIONS):
        raise ValueError("app ELF does not have the expected ESP-IDF memory sections")
    wanted = OTHER_SECTIONS | PLACED_SECTIONS | XIP_SECTIONS.keys()
    return {**dict.fromkeys(CORE_SECTIONS, 0),
            **{name: observed[name] for name in sorted(wanted | set(CORE_SECTIONS)) if name in observed}}


def read_app_elf(out, record):
    # The IDF interpreter already provides pyelftools; importing here keeps dry-run independent.
    from elftools.common.exceptions import ELFError
    from elftools.elf.elffile import ELFFile

    project = json.loads((out / "project_description.json").read_text(encoding="utf-8"))
    if not isinstance(project, dict):
        raise ValueError("malformed project description")
    name = project.get("app_elf")
    if (not isinstance(name, str) or name != Path(name).name or not name.endswith(".elf")
            or project.get("target") != record["target"]):
        raise ValueError("project description does not identify this app ELF")
    expected = record["artifacts"].get(name)
    path = out / name
    if not expected or path.is_symlink():
        raise ValueError("app ELF is not a hashed build artifact")
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
        if digest.hexdigest() != expected:
            raise ValueError("app ELF changed after artifact hashing")
        stream.seek(0)
        try:
            elf = ELFFile(stream)
            if elf["e_type"] != "ET_EXEC":
                raise ValueError("app ELF is not an executable")
            return name, expected, str(elf["e_machine"]), section_sizes(elf.iter_sections())
        except ELFError as exc:
            raise ValueError("app ELF failed to parse") from exc


def comparison_key(record, report, build_kind):
    return {
        "product": record["name"], "board": record["board"], "target": record["target"],
        "entry": record.get("entry", "legacy"), "stage": record["stage"], "build_kind": build_kind,
        "idf": record["idf_identity"], "schema": report["schema"], "machine": report["machine"],
        "psram_xip": report["psram_xip"],
    }


def previous_report(out, key):
    latest = None
    for path in (out / "build-history").glob("attempt-*/build-record.json"):
        try:
            old = json.loads(path.read_text(encoding="utf-8"))
            previous = old["static_memory"]
            metrics = previous["section_bytes"]
            timestamp = previous["completed_at_ns"]
            if (previous["status"] != "available" or previous["comparison_key"] != key
                    or not isinstance(timestamp, int) or not isinstance(metrics, dict)
                    or not all(isinstance(value, int) and value >= 0 for value in metrics.values())
                    or not all(name in metrics for name in CORE_SECTIONS)
                    or old["artifacts"].get(previous["app_elf"]) != previous["elf_sha256"]):
                continue
            if latest is None or timestamp > latest[0]:
                latest = timestamp, path, previous
        except (KeyError, AttributeError, TypeError, OSError, UnicodeError, ValueError):
            continue
    return latest


def summarize(out, record, config, *, completed_at_ns):
    try:
        name, digest, machine, sections = read_app_elf(out, record)
    except (ImportError, OSError, ValueError) as exc:
        return {"status": "unavailable", "schema": SCHEMA, "reason": str(exc)}

    xip = {name: config.get(option) == "y" for name, option in XIP_SECTIONS.items()}
    report = {
        "status": "available", "schema": SCHEMA, "completed_at_ns": completed_at_ns,
        "app_elf": name, "elf_sha256": digest, "machine": machine,
        "section_bytes": {name: size for name, size in sections.items() if name not in XIP_SECTIONS},
        "psram_xip": xip,
        # ELF payload only, not the actual aligned boot-time MMU reservation.
        "psram_xip_section_bytes": {name: sections[name] for name, enabled in xip.items()
                                    if enabled and name in sections},
    }
    report["comparison_key"] = comparison_key(record, report, out.name)
    latest = previous_report(out, report["comparison_key"])
    if latest is not None:
        _, path, previous = latest
        before, after = previous["section_bytes"], report["section_bytes"]
        report["baseline"] = {"record": path.relative_to(out).as_posix(),
                              "elf_sha256": previous["elf_sha256"]}
        report["changes_over_1kib"] = {
            name: {"before": before.get(name, 0), "after": after.get(name, 0), "delta": delta}
            for name in sorted(before.keys() | after.keys())
            if abs(delta := after.get(name, 0) - before.get(name, 0)) > THRESHOLD_BYTES}
    return report


def print_notices(report):
    if report["status"] != "available":
        print(f"NOTE: static memory report unavailable: {report['reason']}")
    for name, change in report.get("changes_over_1kib", {}).items():
        print(f"NOTE: static memory {name}: {change['delta']:+d} B "
              f"({change['before']} -> {change['after']} B)")
