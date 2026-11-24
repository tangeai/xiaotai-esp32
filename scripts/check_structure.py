#!/usr/bin/env python3
"""Static source inventory only: never invokes a compiler, test executable or device."""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import re

from xiaotai_build import variants
import check_sdk_layers
import product_scope

ROOT = Path(__file__).resolve().parents[1]
IGNORED = {".git", "build", "managed_components", "__pycache__", ".local"}
TEXT = {".c", ".cc", ".cpp", ".h", ".inc", ".py", ".cmake", ".json", ".yml", ".md", ".txt", ".defaults"}
PRESERVED = (
    "main/xiaotai/runtime/starter_runtime/", "main/xiaotai/platform/platform_client/",
    "main/xiaotai/platform/nvs_worker/", "main/xiaotai/platform/runtime_config/",
    "main/xiaotai/platform/wifi_manager/", "main/protocols/tirtc/",
    "main/xiaotai/media/", "main/xiaotai/ui/",
)
UNCHANGED_TYPES = {".c", ".cc", ".cpp", ".h", ".inc", ".a", ".json", ".tflite", ".wav", ".png", ".html"}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_files():
    result = []
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [name for name in dirs if name not in IGNORED]
        result.extend(Path(base) / name for name in files if not name.endswith(".pyc"))
    return sorted(result)


def verify_preserved(baseline, relocation):
    preserved = []
    for record in json.loads(baseline.read_text(encoding="utf-8-sig")):
        old = record["path"]
        if not old.startswith(PRESERVED) or Path(old).suffix not in UNCHANGED_TYPES:
            continue
        current = old
        for source, target in relocation.items():
            if old == source or old.startswith(source + "/"):
                current = target + old[len(source):]
                break
        path = ROOT / current
        if not path.is_file() or sha256(path) != record["sha256"].lower():
            raise ValueError(f"Preserved implementation or asset changed: {current}")
        preserved.append({"before": old, "current": current, "sha256": record["sha256"].lower()})
    if len(preserved) < 50:
        raise ValueError("Preservation inventory is incomplete")
    return preserved


def audit(baseline=None, path_map=None):
    files = source_files()
    counts = {suffix: sum(path.suffix == suffix for path in files) for suffix in (".c", ".cc", ".cpp", ".h", ".py")}
    problems = []
    vendor_notices = []
    # The checker itself necessarily contains this denylist; attribution is separate.
    foreign = re.compile(r"xiaozhi|小智|虾哥|tenclass", re.IGNORECASE)
    for path in files:
        rel = path.relative_to(ROOT).as_posix()
        if path.suffix == ".py":
            ast.parse(path.read_text(encoding="utf-8-sig"), filename=rel)
        if path.suffix == ".json":
            json.loads(path.read_text(encoding="utf-8-sig"))
        if rel.startswith("vendor/"):
            # Vendor catalogues and licence/history text remain attributable.
            # Do not rename upstream model identifiers or pretend they are ours.
            if path.suffix in TEXT or path.name.startswith("Kconfig"):
                if foreign.search(path.read_text(encoding="utf-8-sig")):
                    vendor_notices.append(rel)
            continue
        # Attribution files may name the reference project; their links are still checked.
        attribution = rel.startswith(("docs/licenses/", "docs/provenance/")) or rel in {
            "LICENSE", "THIRD_PARTY.md", "scripts/check_structure.py"}
        if not attribution and (path.suffix in TEXT or path.name.startswith(("Kconfig", "sdkconfig"))):
            if foreign.search(path.read_text(encoding="utf-8-sig")):
                problems.append(f"Foreign product content: {rel}")
        if path.suffix == ".md":
            for link in re.findall(r"\[[^\]]*\]\(([^)]+)\)", path.read_text(encoding="utf-8-sig")):
                target = link.split("#", 1)[0]
                if not target or "://" in target or target.startswith("mailto:"):
                    continue
                if not (path.parent / target).exists():
                    problems.append(f"Broken local document link: {rel} -> {target}")

    boards = variants()
    baseline_boards = product_scope.expected_boards({"lckfb/szpi-esp32s3", "m5stack/core-s3",
                                                     "waveshare/esp32-p4-wifi6-touch-lcd-3.5", "wt99p461-s1",
                                                     "xmini/c3-v3"}, ROOT)
    missing_boards = baseline_boards - {board["board"] for board in boards}
    if missing_boards:
        problems.append(f"Preserved product profiles missing: {sorted(missing_boards)}")
    for name in ("scripts/build.py", "scripts/xiaotai_build_native.py", "cmake/esp-idf-product/CMakeLists.txt",
                 "cmake/esp-idf-product/main/main.c"):
        if not (ROOT / name).is_file():
            problems.append(f"Missing build entry: {name}")
    for stale in ("main", "CMakeLists.txt", "scripts/boards.py", "scripts/product_contract.py"):
        if (ROOT / stale).exists():
            problems.append(f"Legacy entry is back: {stale}")
    # SDK code reaches the platform only through the port layer.
    problems.extend(check_sdk_layers.problems(ROOT))

    # Every TiRTC package listed in packages.json must match its contract exactly.
    sdk = {}
    sdk_root = ROOT / "components/tirtc_sdk"
    table = json.loads((sdk_root / "packages.json").read_text(encoding="utf-8-sig"))
    if any((sdk_root / old).exists() for old in ("lib", "idf5.5", "variants/szpi-http")):
        problems.append("TiRTC archives are back in the tree; official packages are downloaded (scripts/fetch_tirtc.py)")
    for line, spec in table["lines"].items():
        for name, entry in spec["packages"].items():
            key = f"{line}/{name}"
            if "path" in entry:
                # Downloaded official package: its headers are kept once in include/
                # for host tests; the archive is checked when it has been fetched.
                unpacked = sdk_root / "packages" / entry["path"].rsplit("/", 1)[-1]
                lib, include = unpacked / "lib", sdk_root / "include"
                contract = json.loads((sdk_root / entry["contract"]).read_text(encoding="utf-8-sig"))
                sdk[key] = contract["sha256"]
                if (lib / "libTiRTC.a").is_file() and sha256(lib / "libTiRTC.a") != contract["sha256"]:
                    problems.append(f"SDK identity changed: {key}")
            else:
                lib, include = sdk_root / entry["lib"], sdk_root / entry["include"]
                contract = json.loads((sdk_root / entry.get("contract", entry["lib"] + "/contract.json")).read_text(encoding="utf-8-sig"))
                sdk[key] = sha256(lib / "libTiRTC.a")
                if sdk[key] != contract["sha256"]:
                    problems.append(f"SDK identity changed: {key}")
            expected_target = name.removeprefix("generic-") if name.startswith("generic-") else contract["target"]
            if contract["target"] != expected_target:
                problems.append(f"SDK identity changed: {key}")
            for header, digest in contract.get("headers", {}).items():
                if sha256(include / "tirtc" / header) != digest:
                    problems.append(f"SDK header changed: {key} {header}")
            if "xmini_contract" in contract:
                diagnostic = ROOT / "sdk/xiaotai_transport/esp-idf/include/tgtrp_internal_diag.h"
                if sha256(diagnostic) != contract["xmini_contract"]["diagnostic_header_sha256"]:
                    problems.append("Xmini diagnostic ABI changed")
    preserved = verify_preserved(baseline, json.loads(path_map.read_text(encoding="utf-8-sig"))) if baseline and path_map else []
    if problems:
        raise ValueError("\n".join(problems))
    return {"static_checks_passed": True,
            "checks_executed_by_this_command": ["static-inventory", "source-contracts", "sdk-identities"],
            "source_file_count": len(files), "language_file_counts": counts,
            "vendor_attribution_or_catalogue_files": vendor_notices,
            "board_profiles": len(boards), "sdk_sha256": sdk,
            "preserved_file_count": len(preserved), "preserved_files": preserved}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--path-map", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if bool(args.baseline) != bool(args.path_map):
        parser.error("--baseline and --path-map must be provided together")
    result = audit(args.baseline, args.path_map)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    summary = {key: value for key, value in result.items() if key != "preserved_files"}
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
