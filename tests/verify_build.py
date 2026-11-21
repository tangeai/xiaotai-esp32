"""Read-only artifact audit; run in the same environment as the recorded build."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import sdk_selection
from flash_layout import flash_layout
from xiaotai_build import verify_dependency_lock
from xiaotai_build_native import dependency_lock_path, load_profile, resolved_lock_path, verify_variant_lock


ROOT = Path(__file__).resolve().parents[1]
S3_BUILD_OPTIONS = {"development_console", "ai_vision", "h5_video"}
S3_CONSOLE_SYMBOLS = {
    "starter_console_start", "xiaotai_at_execute", "xiaotai_ui_tap",
    "xiaotai_ui_sleep", "xiaotai_ui_inspect_preferences", "starter_runtime_ai_state_read",
}
CORES3_CONSOLE_SYMBOLS = {
    "starter_console_start", "starter_runtime_ai_state_read",
    "xiaotai_ui_capture", "lv_snapshot_take_to_draw_buf",
}
# esp_image_header_t: magic, chip_id, min_chip_rev_full, max_chip_rev_full.
ESP_IMAGE_REVISION_HEADER = struct.Struct("<B11xHxHH5x")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_p4_revision(folder, layout):
    raw = (folder / "flasher_args.json").read_bytes()
    if hashlib.sha256(raw).hexdigest() != layout["flasher_args_sha256"]:
        raise ValueError("Flash arguments changed during P4 revision check")
    args = json.loads(raw)
    images = [args[section]["file"] for section in ("bootloader", "app")]
    if images[0] == images[1]:
        raise ValueError("P4 bootloader and app must be separate images")
    ranges = []
    for name in images:
        with (folder / name).open("rb") as image:
            header = image.read(ESP_IMAGE_REVISION_HEADER.size)
        if len(header) != ESP_IMAGE_REVISION_HEADER.size:
            raise ValueError(f"Short ESP image header: {name}")
        magic, chip, minimum, maximum = ESP_IMAGE_REVISION_HEADER.unpack(header)
        if magic != 0xE9 or chip != 0x0012:
            raise ValueError(f"{name} is not an ESP32-P4 image")
        if not 301 <= minimum <= maximum <= 399:
            raise ValueError(f"{name} chip revision range {minimum}-{maximum} is outside v3.1-v3.99")
        ranges.append((minimum, maximum))
    minimum = max(lower for lower, _ in ranges)
    maximum = min(upper for _, upper in ranges)
    if minimum > maximum:
        raise ValueError("P4 bootloader and app have no common chip revision")
    return {"min": minimum, "max": maximum}


def verify_console_symbols(defined, expected, enabled):
    linked = expected & defined
    if enabled and linked != expected:
        raise ValueError(f"Missing development console symbols: {', '.join(sorted(expected - linked))}")
    if not enabled and linked:
        raise ValueError(f"Unexpected development console symbols: {', '.join(sorted(linked))}")


def verify_s3_build_options(record, sdkconfig, compiled, defined, expected):
    if (not isinstance(expected, dict) or not expected or set(expected) - S3_BUILD_OPTIONS
            or any(type(value) is not bool for value in expected.values())):
        raise ValueError("Expected a nonempty JSON object of boolean S3 build options")
    paths = {str(path).replace("\\", "/") for path in compiled}

    def includes(path):
        return any(file.endswith("/" + path) or file == path for file in paths)

    if record["stage"] != "firmware" or record["target"] != "esp32s3":
        raise ValueError("S3 build option audit requires an ESP32-S3 firmware")

    # Both S3 boards run the shared runtime; CoreS3-Lite keeps its own media
    # stack until the S3 media layer is unified.
    runtime = includes("main/xiaotai/application/s3/starter_runtime/src/starter_runtime.c")
    cores3_media = includes("main/xiaotai/media/cores3/starter_media/src/starter_media.c")
    is_cores3 = record.get("board") == "m5stack/core-s3"
    if is_cores3 and (not runtime or not cores3_media):
        raise ValueError("S3 build option audit requires a CoreS3-Lite firmware source graph for this board")
    if not is_cores3 and (not runtime or cores3_media):
        raise ValueError("S3 build option audit requires the lckfb S3 firmware source graph for this board")
    supported = {"development_console"} if is_cores3 else S3_BUILD_OPTIONS
    if set(expected) - supported:
        raise ValueError("CoreS3-Lite audit only accepts development_console")

    values = dict(line.split("=", 1) for line in sdkconfig.splitlines()
                  if line.startswith("CONFIG_") and "=" in line)
    effective = {key: values.get(f"CONFIG_XIAOTAI_{key.upper()}") == "y"
                 for key in sorted(supported)}
    for key, value in expected.items():
        if effective[key] != value:
            raise ValueError(f"Effective S3 option {key} differs: expected {value}, got {effective[key]}")

    console = effective["development_console"]
    if is_cores3:
        console_source = includes("main/xiaotai/input/cores3/starter_console/src/starter_console.c")
        snapshot = values.get("CONFIG_LV_USE_SNAPSHOT") == "y"
        if console_source != console or snapshot != console:
            raise ValueError("CoreS3-Lite console source or LVGL snapshot disagrees with the effective option")
        verify_console_symbols(defined, CORES3_CONSOLE_SYMBOLS, console)
        if not {"starter_button_start", "starter_product_start", "starter_tirtc_start"} <= defined:
            raise ValueError("CoreS3-Lite is missing required runtime symbols")
        return effective

    required = ("AI", "DEVICE_CALL", "WECHAT_VOIP", "ROOM", "H5_STREAM")
    if any(values.get(f"CONFIG_XIAOTAI_{key}") != "y" for key in required):
        raise ValueError("lckfb S3 firmware is missing required voice features")

    verify_console_symbols(defined, S3_CONSOLE_SYMBOLS, console)

    camera = effective["ai_vision"] or effective["h5_video"]
    if (includes("main/xiaotai/media/s3/starter_media/src/starter_camera.c") != camera or
            includes("main/xiaotai/media/s3/starter_media/camera_disabled.c") == camera or
            ("xiaotai_board_camera_set_power" in defined) != camera):
        raise ValueError("S3 camera source or power entry disagrees with effective build options")
    return effective


def verify_native_variant(record, manifest, defined, resolved):
    variant = record["variant"]
    if not isinstance(variant, str):
        raise ValueError("Invalid native build variant")
    name = record["name"]
    product = sdk_selection.load_product(ROOT / "products" / name / "product.json")
    selected = sdk_selection.plan(ROOT, sdk_selection.apply_variant(product, variant))
    apps, modules, _ = selected.pop("units")
    if selected != record.get("selection"):
        raise ValueError(f"Recorded product selection does not match the {variant} variant")
    line = record["idf_line"]
    product_lock = dependency_lock_path(ROOT, name, line=line)
    reduced = bool(product["variants"][variant].get("without"))
    pinned = dependency_lock_path(ROOT, name, variant, line=line) if reduced else product_lock
    if (record.get("dependency_lock") != pinned.relative_to(ROOT).as_posix() or
            any(str(path) not in manifest for path in (pinned, product_lock)) or
            (reduced and record.get("product_dependency_lock") != product_lock.relative_to(ROOT).as_posix())):
        raise ValueError(f"Recorded dependency lock does not match the {variant} variant")
    if reduced:
        verify_variant_lock(product_lock, pinned)
    verify_dependency_lock(pinned, resolved)

    def symbols(unit):
        prefix = "app" if unit.kind == "application" else "engine" if unit.engine else "module"
        name = f"xiaotai_{prefix}_{unit.name}"
        return {f"{name}_init", f"{name}_start"} if unit.start else {f"{name}_init"}

    required = set().union(*(symbols(unit) for unit in apps + modules))
    all_units = sdk_selection.catalog(ROOT)
    declared = set().union(*(symbols(unit) for group in all_units.values() for unit in group.values()))
    missing = required - defined
    unexpected = (declared - required) & defined
    if missing or unexpected:
        raise ValueError(f"SDK-native variant symbols disagree: missing={sorted(missing)}, unexpected={sorted(unexpected)}")


def verify(folder, require_flash_layout=False, expected_board=None, expected_s3_options=None, expected_entry=None,
           expected_variant=None):
    record = json.loads((folder / "build-record.json").read_text())
    entry = record.get("entry", "legacy")
    if entry not in {"legacy", "sdk"} or (expected_entry is not None and entry != expected_entry):
        raise ValueError(f"Build entry differs: expected {expected_entry}, got {entry}")
    native = entry == "sdk"
    if expected_variant is not None and (not native or record.get("variant") != expected_variant):
        raise ValueError(f"Build variant differs: expected {expected_variant}, got {record.get('variant')}")
    named_variant = re.fullmatch(r"sdk-firmware(?:-idf[0-9]+\.[0-9]+)?--([a-z0-9-]+)", folder.name)
    if native and named_variant and record.get("variant") != named_variant[1]:
        raise ValueError("SDK-native build directory and recorded variant differ")
    if native:
        profile = load_profile(record["name"], line=record.get("idf_line"))
        identity = (record["stage"], record["target"], record.get("board"), record.get("idf_line"),
                    record.get("idf_environment", {}).get("version"), record.get("tirtc_package"))
        expected = ("firmware", profile["target"], profile["board"], profile["idf_line"],
                    profile["idf_version"], profile["tirtc_package"])
        if identity != expected:
            raise ValueError("Unexpected SDK-native product, target, IDF or TiRTC package")
    if native and expected_s3_options is not None:
        raise ValueError("Legacy S3 build options do not describe the SDK-native product")
    app = record["name"].replace("-", "_") if native else "xiaotai"
    if expected_board is not None and record.get("board") != expected_board:
        raise ValueError(f"Build board differs: expected {expected_board}, got {record.get('board')}")
    if expected_s3_options is not None and record["stage"] != "firmware":
        raise ValueError("S3 build option audit requires a firmware build")
    layout = None
    if record["stage"] == "firmware" and "flash_layout" in record:
        layout = flash_layout(folder, record["target"], app_name=f"{app}.bin")
        if layout != record["flash_layout"]:
            raise ValueError(f"Flash layout changed: {folder}")
    elif require_flash_layout or native:
        raise ValueError(f"No recorded firmware flash layout: {folder}")
    p4_revision = None
    if require_flash_layout and record["target"] == "esp32p4":
        p4_revision = verify_p4_revision(folder, layout)
    assert record["hardware_tested"] is False
    assert sha256(Path(record["sdkconfig"])) == record["sdkconfig_sha256"], "sdkconfig changed"
    for name, expected in record["artifacts"].items():
        assert sha256(folder / name) == expected, f"Artifact changed: {name}"
    if native and record["idf_line"] == "5.5" and (
            record["artifacts"].get("xiaotai.bin") != record["artifacts"][f"{app}.bin"]):
        raise ValueError("SDK-native xiaotai.bin does not match the audited APP image")
    manifest_path = folder / "build-inputs.json"
    assert sha256(manifest_path) == record["input_manifest_sha256"], "Input manifest changed"
    manifest = json.loads(manifest_path.read_text())
    assert len(manifest) == record["input_file_count"]
    for name in ("build.ninja", "compile_commands.json", "CMakeCache.txt", "config/sdkconfig.h", "toolchain/cflags"):
        assert str(folder / name) in manifest, f"Missing effective build input: {name}"
    for name, expected in manifest.items():
        assert sha256(Path(name)) == expected, f"Build input changed: {name}"
    if native or "dependencies_lock_sha256" in record:
        lock = resolved_lock_path(folder, profile) if native else folder / "dependencies.lock"
        assert sha256(lock) == record["dependencies_lock_sha256"], "Resolved dependencies changed"
    cache = (folder / "CMakeCache.txt").read_text()
    nm = next(line.split("=", 1)[1] for line in cache.splitlines() if line.startswith("CMAKE_NM:FILEPATH="))
    elf = folder / f"{app}.elf" if native else next(folder.glob("*.elf"))
    symbols = subprocess.check_output([nm, "--defined-only", "--extern-only", str(elf)], text=True)
    defined = {line.split()[-1] for line in symbols.splitlines() if len(line.split()) >= 3}
    result = {"name": record["name"], "stage": record["stage"],
              "input_file_count": len(manifest), "artifacts": record["artifacts"],
              "hardware_tested": False}
    if record["stage"] == "sdk-check":
        roots = set((folder / "sdk-link-roots.txt").read_text().splitlines())
        assert len(roots) > 1000, "Incomplete SDK link roots"
        assert roots <= defined, f"Discarded SDK functions: {sorted(roots - defined)}"
        result["retained_sdk_functions"] = len(roots)
    else:
        result["flash_layout_verified"] = layout is not None
        if p4_revision is not None:
            result["p4_chip_revision_range"] = p4_revision
        commands = json.loads((folder / "compile_commands.json").read_text())
        compiled = {entry["file"] for entry in commands}
        forbidden = ("main/application.cc", "main/protocols/mqtt_protocol.cc",
                     "main/protocols/websocket_protocol.cc", "main/mcp_server.cc", "main/ota.cc")
        assert not any(path.endswith(forbidden) for path in compiled), "Unexpected business owner entered product"
        demangled = subprocess.check_output([nm, "-C", "--defined-only", str(elf)], text=True)
        factories = [line for line in demangled.splitlines() if line.endswith(" T xiaotai::board::SelectedBoard()")]
        assert len(factories) == 1, f"Expected one Board factory: {factories}"
        result["board_factories"] = len(factories)
        result["unexpected_business_compiled"] = False
        if native:
            if not {str(folder / "project/main/main.c"), str(folder / "generated/xiaotai_selected.c")} <= compiled:
                raise ValueError("Missing SDK-native app_main or generated product source")
            if any("/main/xiaotai/" in path.replace("\\", "/") for path in compiled):
                raise ValueError("Legacy application entered SDK-native build")
            app_mains = [line for line in demangled.splitlines() if line.endswith(" T app_main")]
            if len(app_mains) != 1 or not {"xiaotai_start", "xiaotai_at_service", "TiRtcInit"} <= defined:
                raise ValueError("SDK-native runtime symbols are missing or ambiguous")
            if "variant" in record:
                verify_native_variant(record, manifest, defined, lock)
                result["variant"] = record["variant"]
                result["variant_symbols_verified"] = True
            # IDF 5.5 ships esptool 4, whose extended output is opt-in.
            image_info = ["image_info", "--version", "2"] if record["idf_line"] == "5.5" else ["image-info"]
            image = subprocess.check_output(
                [sys.executable, "-m", "esptool", "--chip", record["target"], *image_info,
                 str(folder / f"{app}.bin")], text=True)
            if (f"ELF file SHA256: {record['artifacts'][elf.name]}" not in image.splitlines()
                    or not any(line.startswith("Validation hash:") and line.endswith("(valid)")
                               for line in image.splitlines())):
                raise ValueError("SDK-native APP image identity does not match the audited ELF")
            result["entry"] = entry
            result["elf_image_identity_verified"] = True
        if expected_s3_options is not None:
            result["s3_build_options_verified"] = verify_s3_build_options(
                record, Path(record["sdkconfig"]).read_text(), compiled, defined, expected_s3_options)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-flash-layout", action="store_true",
                        help="Require an audited flash layout and rev3-only image headers on P4")
    parser.add_argument("--board", help="Require the recorded board to match this build selector")
    parser.add_argument("--entry", choices=("legacy", "sdk"), help="Require the recorded build entry")
    parser.add_argument("--variant", help="Require this recorded SDK-native variant")
    parser.add_argument("--expect-s3-build-options-json",
                        help="Verify the lckfb S3 feature options or the CoreS3-Lite development console")
    parser.add_argument("build_dirs", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        expected_s3_options = None
        if args.expect_s3_build_options_json is not None:
            expected_s3_options = json.loads(args.expect_s3_build_options_json)
            if expected_s3_options is None:
                raise ValueError("Expected a nonempty JSON object of boolean S3 build options")
        results = [verify(folder.resolve(), args.require_flash_layout, args.board, expected_s3_options, args.entry,
                          args.variant)
                   for folder in args.build_dirs]
    except (ValueError, OSError) as error:
        parser.exit(1, f"ERROR: {error}\n")
    print(json.dumps({"passed": True, "builds": results}, indent=2))


if __name__ == "__main__":
    main()
