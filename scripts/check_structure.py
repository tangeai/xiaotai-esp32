#!/usr/bin/env python3
"""Static source inventory only: never invokes a compiler, test executable or device."""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import re

from xiaotai_build import configuration, variants

ROOT = Path(__file__).resolve().parents[1]
IGNORED = {".git", "build", "managed_components", "__pycache__", ".local"}
TEXT = {".c", ".cc", ".cpp", ".h", ".inc", ".py", ".cmake", ".json", ".yml", ".md", ".txt", ".defaults"}
PRESERVED = (
    "main/xiaotai/runtime/starter_runtime/", "main/xiaotai/platform/platform_client/",
    "main/xiaotai/platform/nvs_worker/", "main/xiaotai/platform/runtime_config/",
    "main/xiaotai/platform/wifi_manager/", "main/protocols/tirtc/",
    "main/xiaotai/media/", "main/xiaotai/ui/", "components/tirtc_sdk/lib/",
    "components/tirtc_sdk/include/",
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
        if rel.startswith(("docs/licenses/", "docs/provenance/")) or rel in {"THIRD_PARTY.md", "scripts/check_structure.py"}:
            continue
        if rel.startswith("vendor/"):
            # Vendor catalogues and licence/history text remain attributable.
            # Do not rename upstream model identifiers or pretend they are ours.
            if path.suffix in TEXT or path.name.startswith("Kconfig"):
                if foreign.search(path.read_text(encoding="utf-8-sig")):
                    vendor_notices.append(rel)
            continue
        if path.suffix in TEXT or path.name.startswith(("Kconfig", "sdkconfig")):
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
    baseline_boards = {"lckfb/szpi-esp32s3", "m5stack/core-s3", "mz28-ai-ppd",
                       "waveshare/esp32-p4-wifi6-touch-lcd-3.5", "wt99p461-s1", "xmini/c3-v3"}
    missing_boards = baseline_boards - {board["board"] for board in boards}
    if missing_boards:
        problems.append(f"Preserved product profiles missing: {sorted(missing_boards)}")
    for board in boards:
        plan = configuration(board, "sdk-check")
        if board["target"] == "esp32c3" and plan["CONFIG_XIAOTAI_H5_VIDEO"] != "n":
            problems.append(f"C3 video contract: {board['name']}")
    xmini = next(board for board in boards if board["board"] == "xmini/c3-v3")
    if "h5_stream" not in xmini["xiaotai"]["features"]:
        problems.append("Xmini must preserve web real-time audio")
    mz28 = next(board for board in boards if board["board"] == "mz28-ai-ppd")
    if mz28["xiaotai"]["features"] != ["wechat_voip"]:
        problems.append("MZ28 feature baseline changed")

    required = (
        "main/main.cc", "main/boards/common/board.h", "main/boards/common/board_bridge.cc",
        "main/boards/lckfb/szpi-esp32s3/board.cmake", "main/boards/lckfb/szpi-esp32s3/board.cc",
        "main/boards/lckfb/szpi-esp32s3/xiaotai_hardware.c",
        "main/xiaotai/application/application.cc", "main/xiaotai/application/startup_services.c",
        "main/xiaotai/application/session/src/starter_runtime.c",
        "main/xiaotai/services/device_platform/src/platform_client.c",
        "main/xiaotai/transport/tirtc/src/starter_tirtc.c",
    )
    for name in required:
        if not (ROOT / name).is_file():
            problems.append(f"Missing implementation: {name}")
    cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8-sig")
    for stale in ("audio/audio_codec.cc", "settings.cc", "xiaotai_board.cc", "application_boot.c"):
        if stale in cmake:
            problems.append(f"Stale build source: {stale}")
    bridge = (ROOT / "main/boards/common/board.h").read_text(encoding="utf-8-sig")
    if "GenerateUuid" in bridge or "GetNetwork" in bridge or "create_board" in bridge:
        problems.append("Board still mixes device identity/network/product ownership")
    launch = (ROOT / "main/xiaotai/application/startup_services.c").read_text(encoding="utf-8-sig").split("esp_err_t xiaotai_startup_launch_services(void)", 1)[1]
    if launch.index("s_tirtc_internal_reserve = heap_caps_malloc(") > launch.index("xTaskCreateStaticPinnedToCore("):
        problems.append("Startup worker can run before reserve publication")

    sdk = {}
    for target in ("esp32s3", "esp32c3", "esp32p4"):
        folder = ROOT / "components/tirtc_sdk/lib" / target
        contract = json.loads((folder / "contract.json").read_text(encoding="utf-8-sig"))
        sdk[target] = sha256(folder / "libTiRTC.a")
        if sdk[target] != contract["sha256"] or contract["target"] != target:
            problems.append(f"SDK identity changed: {target}")
    mini = ROOT / "components/tirtc_sdk/variants/xmini"
    contract = json.loads((mini / "lib/contract.json").read_text(encoding="utf-8-sig"))
    sdk["xmini-mini"] = sha256(mini / "lib/libTiRTC.a")
    if sdk["xmini-mini"] != contract["sha256"] or contract["target"] != "esp32c3":
        problems.append("Xmini SDK identity changed")
    for name, digest in contract["headers"].items():
        if sha256(mini / "include/tirtc" / name) != digest:
            problems.append(f"Xmini SDK header changed: {name}")
    diagnostic = ROOT / "main/xiaotai/transport/xmini/xmini_rtc/include/tgtrp_internal_diag.h"
    if sha256(diagnostic) != contract["xmini_contract"]["diagnostic_header_sha256"]:
        problems.append("Xmini diagnostic ABI changed")
    szpi = ROOT / "components/tirtc_sdk/variants/szpi-http"
    contract = json.loads((szpi / "lib/contract.json").read_text(encoding="utf-8-sig"))
    sdk["szpi-http"] = sha256(szpi / "lib/libTiRTC.a")
    if sdk["szpi-http"] != contract["sha256"] or contract["target"] != "esp32s3":
        problems.append("SZPI SDK identity changed")
    for name, digest in contract["headers"].items():
        if sha256(szpi / "include/tirtc" / name) != digest:
            problems.append(f"SZPI SDK header changed: {name}")
    state = json.loads((ROOT / "config/development-status.json").read_text(encoding="utf-8-sig"))
    preserved = verify_preserved(baseline, json.loads(path_map.read_text(encoding="utf-8-sig"))) if baseline and path_map else []
    if problems:
        raise ValueError("\n".join(problems))
    return {"static_checks_passed": True, "phase": state["phase"],
            "preparation_complete": state["preparation_complete"],
            "checks_executed_by_this_command": ["static-inventory", "source-contracts", "sdk-identities"],
            "source_file_count": len(files), "language_file_counts": counts,
            "vendor_attribution_or_catalogue_files": vendor_notices,
            "board_profiles": len(boards), "sdk_sha256": sdk,
            "preserved_file_count": len(preserved), "preserved_files": preserved,
            "pending": state["pending"]}


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
