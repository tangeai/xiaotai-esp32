"""Resolve XiaoTai product profiles and isolate their effective build inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from product_contract import contract_paths, validate_variant
from idf_environment import LOCKS as IDF_LOCKS, verify as verify_idf

ROOT = Path(__file__).resolve().parents[1]
FEATURES = ("ai", "device_call", "wechat_voip", "room", "h5_stream", "h5_video", "ai_vision")
TARGETS = {"esp32s3", "esp32p4", "esp32c3"}


def variants(root=ROOT):
    result = []
    names = set()
    for path in sorted((root / "main/boards").rglob("config.json")):
        config = json.loads(path.read_text(encoding="utf-8-sig"))
        for build in config.get("builds", []):
            profile = build.get("xiaotai")
            if profile is None:
                raise ValueError(f"Missing XiaoTai product contract: {path}")
            name = build["name"]
            if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", name) or name in names:
                raise ValueError(f"Invalid or duplicate build name: {name}")
            names.add(name)
            if config["target"] not in TARGETS:
                raise ValueError(f"No TiRTC SDK for {config['target']}")
            if profile.get("schema") != 1 or set(profile["features"]) - set(FEATURES):
                raise ValueError(f"Invalid XiaoTai profile: {path}")
            if "ai_vision" in profile["features"] and "ai" not in profile["features"]:
                raise ValueError(f"AI vision requires AI: {name}")
            if "h5_video" in profile["features"] and "h5_stream" not in profile["features"]:
                raise ValueError(f"H5 video requires the H5 session: {name}")
            if config["target"] == "esp32c3" and {"h5_video", "ai_vision"} & set(profile["features"]):
                raise ValueError(f"C3 profiles have no video backend: {name}")
            variant = dict(board=path.parent.relative_to(root / "main/boards").as_posix(),
                           target=config["target"], **build)
            validate_variant(variant, root)
            result.append(variant)
    return result


def select(board, name=None, root=ROOT):
    matches = [v for v in variants(root) if v["board"] == board and (name is None or v["name"] == name)]
    if len(matches) != 1:
        raise ValueError("Select one registered XiaoTai board/name from --list-boards")
    return matches[0]


def read_config(path):
    result = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            result[key] = value
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            result[line[2:-11]] = "n"
    return result


def sdk_directory(variant, root=ROOT):
    sdk = root / "components/tirtc_sdk"
    if variant["board"] == "xmini/c3-v3":
        return sdk / "variants/xmini/lib"
    if variant["board"] == "lckfb/szpi-esp32s3":
        return sdk / "variants/szpi-http/lib"
    return sdk / "lib" / variant["target"]


def configuration(variant, stage, options=None, root=ROOT):
    validate_variant(variant, root)
    options = {} if options is None else options
    if not isinstance(options, dict) or set(options) - (set(FEATURES) | {"development_console"}):
        raise ValueError("Only documented feature booleans are accepted")
    enabled = set(variant["xiaotai"]["features"])
    for key, value in options.items():
        if type(value) is not bool:
            raise ValueError(f"{key} must be a JSON boolean")
        if key == "development_console":
            if stage != "firmware" or variant["board"] != "lckfb/szpi-esp32s3":
                raise ValueError("development_console adapter is currently validated only for SZPI S3 firmware")
            continue
        if value != (key in enabled):
            raise ValueError(f"{key}: source/UI/capability trimming has not been validated; use this board's preserved defaults")
    values = read_config(root / "config/xiaotai.defaults")
    sdk_defaults = sdk_directory(variant, root) / "sdkconfig.defaults"
    if sdk_defaults.is_file():
        values.update(read_config(sdk_defaults))
    if stage == "sdk-check":
        contract = json.loads((sdk_directory(variant, root) / "contract.json").read_text(encoding="utf-8-sig"))
        for key, value in contract["crypto_config"].items():
            values[key] = value if re.fullmatch(r"y|n|[0-9]+|0x[0-9a-fA-F]+", value) else json.dumps(value)
        values.update(contract.get("atomic_config", {}))
        values.update(contract.get("libc_config", {}))
    if stage == "firmware":
        if variant["xiaotai"]["status"] not in {"migrating", "source-migrated", "build-verified"}:
            raise ValueError(f"{variant['name']}: Board/UI/media migration pending; --stage sdk-check is not product firmware")
        defaults = root / "main/boards" / variant["board"] / "sdkconfig.xiaotai"
        values.update(read_config(defaults))
    for assignment in variant["sdkconfig_append"]:
        key, value = assignment.split("=", 1)
        values[key] = value
    values["CONFIG_IDF_TARGET"] = json.dumps(variant["target"])
    values["CONFIG_XIAOTAI_APP"] = "y"
    for feature in FEATURES:
        values[f"CONFIG_XIAOTAI_{feature.upper()}"] = "y" if feature in enabled else "n"
    if "development_console" in options:
        values["CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE"] = "y" if options["development_console"] else "n"
    return values


def plan(variant, stage, options=None, root=ROOT):
    target = os.environ.get("IDF_TARGET")
    if target and target != variant["target"]:
        raise ValueError(f"IDF_TARGET={target} conflicts with {variant['target']}")
    config = configuration(variant, stage, options, root)
    build_kind = stage + ("-diagnostics" if options and options.get("development_console") else "")
    build = root / "build" / variant["name"] / build_kind
    sdk = sdk_directory(variant, root)
    contract = json.loads((sdk / "contract.json").read_text(encoding="utf-8"))
    version = contract["idf_version"]
    if version not in IDF_LOCKS:
        raise ValueError(f"No validated IDF environment for {version}")
    return dict(name=variant["name"], board=variant["board"], target=variant["target"],
                stage=stage, build_dir=str(build), sdkconfig=str(build / "sdkconfig"),
                sdk_directory=sdk.relative_to(root).as_posix(),
                idf_environment={"version": version, **IDF_LOCKS[version]},
                config=config, compatibility=validate_variant(variant, root), hardware_tested=False)


def dependency_lock(variant, root=ROOT):
    return root / "profiles/dependency-locks" / (variant["name"] + ".lock")


def seed_dependency_lock(variant, out, root=ROOT):
    source = dependency_lock(variant, root)
    if not source.is_file():
        raise ValueError(f"Missing product dependency lock: {source}")
    destination = out / "dependencies.lock"
    if not destination.exists():
        shutil.copyfile(source, destination)


def verify_config(actual, expected):
    foreign_wake = [key for key, value in actual.items()
                    if value == "y" and key.startswith("CONFIG_SR_WN_")
                    and "XIAO3ZHI4" in key]
    if foreign_wake:
        raise ValueError(f"Non-product wake model selected: {foreign_wake}")
    rejected = {k: (v, actual.get(k)) for k, v in expected.items()
                if actual.get(k, "n") != v}
    if rejected:
        raise ValueError(f"Effective sdkconfig rejected requested values: {rejected}")


def require_validation_phase(root=ROOT):
    status = json.loads((root / "config/development-status.json").read_text(encoding="utf-8"))
    if status.get("phase") == "preparation" and status.get("preparation_checks_authorized") is True:
        return
    if status.get("phase") != "validation" or status.get("preparation_complete") is not True or status.get("pending"):
        raise ValueError("Preparation is not complete and build/host checks are not authorized; see docs/VALIDATION.md")


def archive_build_record(out):
    records = [out / name for name in ("build-record.json", "build-inputs.json")]
    if not any(path.exists() for path in records):
        return None
    history = out / "build-history"
    history.mkdir(parents=True, exist_ok=True)
    previous = Path(tempfile.mkdtemp(prefix="attempt-", dir=history))
    for path in records:
        if path.exists():
            path.replace(previous / path.name)
    return previous


def request_cmake_inputs(out):
    query = out / ".cmake/api/v1/query/cmakeFiles-v1"
    query.parent.mkdir(parents=True, exist_ok=True)
    query.write_text("")


def cmake_input_paths(out):
    reply = out / ".cmake/api/v1/reply"
    indexes = list(reply.glob("index-*.json"))
    if not indexes:
        raise ValueError("CMake input inventory is missing")
    index = max(indexes, key=lambda path: path.stat().st_mtime_ns)
    response = json.loads(index.read_text())["reply"]["cmakeFiles-v1"]
    inventory = reply / response["jsonFile"]
    data = json.loads(inventory.read_text())
    if data["kind"] != "cmakeFiles" or Path(data["paths"]["build"]).resolve() != out.resolve():
        raise ValueError("CMake input inventory belongs to another build")
    source = Path(data["paths"]["source"])
    paths = {index, inventory, out / ".cmake/api/v1/query/cmakeFiles-v1"}
    for item in data["inputs"]:
        path = Path(item["path"])
        path = path if path.is_absolute() else source / path
        if path.is_file():
            paths.add(path.resolve())
        elif not path.is_dir() and not path.resolve().is_relative_to(out.resolve()):
            raise ValueError(f"CMake input disappeared: {path}")
    return paths


def build_input_paths(out):
    # CMake represents its inputs as phony edges, which Ninja inputs omits.
    # The file API supplies those files without parsing generator-specific text.
    paths = cmake_input_paths(out)
    inputs = subprocess.check_output(["ninja", "-C", str(out), "-t", "inputs", "all"], text=True)
    deps = subprocess.check_output(["ninja", "-C", str(out), "-t", "deps"], text=True)
    header_inputs = {line.strip() for line in deps.splitlines() if line.startswith("    ")}
    if not header_inputs:
        raise ValueError("Compiler dependency inventory is empty")
    # Ninja 1.11 shell-quotes inputs and has no newer -E switch. Its dependency
    # listing, in contrast, already contains literal paths.
    ninja_inputs = set()
    for line in inputs.splitlines():
        if line.startswith(("'", '"')):
            parts = shlex.split(line, posix=os.name != "nt")
            if len(parts) != 1:
                raise ValueError(f"Unexpected Ninja input path: {line}")
            line = parts[0]
            if os.name == "nt" and line.startswith('"') and line.endswith('"'):
                line = line[1:-1]
        ninja_inputs.add(line)
    for line in ninja_inputs | header_inputs:
        path = Path(line)
        path = path if path.is_absolute() else out / path
        if path.is_file():
            paths.add(path.resolve())
    return paths


def run(variant, stage, options=None, fresh_config=False):
    require_validation_phase()
    info = plan(variant, stage, options)
    idf = Path(os.environ.get("IDF_PATH", ""))
    idf_identity = verify_idf(idf, info["idf_environment"]["version"])
    out = Path(info["build_dir"])
    out.mkdir(parents=True, exist_ok=True)
    # IDF's managed_components cache is project-wide even with per-variant locks.
    # Serialize firmware dependency resolution/builds until the cache is immutable.
    lock = ROOT / "build/.firmware-build.lock" if stage == "firmware" else out / ".xiaotai-build.lock"
    try:
        handle = lock.open("x")
    except FileExistsError as error:
        raise ValueError(f"Build already locked: {lock}; inspect the owning process before removing") from error
    try:
        handle.write(str(os.getpid()))
        handle.close()
        # A failed attempt must never leave a previous success as the current record.
        archive_build_record(out)
        requested = out / "requested.json"
        serialized = json.dumps(info, sort_keys=True, indent=2) + "\n"
        if requested.exists() and requested.read_text() != serialized and not fresh_config:
            raise ValueError("Profile inputs changed; use --fresh-config to archive and regenerate only this profile's sdkconfig")
        if fresh_config and Path(info["sdkconfig"]).exists():
            previous = out / "configuration-history" / hashlib.sha256(Path(info["sdkconfig"]).read_bytes()).hexdigest()
            previous.mkdir(parents=True, exist_ok=True)
            Path(info["sdkconfig"]).replace(previous / "sdkconfig")
            if requested.exists():
                requested.replace(previous / "requested.json")
        requested.write_text(serialized, encoding="utf-8")
        defaults = out / "sdkconfig.defaults"
        defaults.write_text("\n".join(f"{k}={v}" for k, v in info["config"].items()) + "\n", encoding="utf-8")
        if stage == "firmware":
            seed_dependency_lock(variant, out)
        project = ROOT if stage == "firmware" else ROOT / "tests/sdk_link"
        args = [sys.executable, str(idf / "tools/idf.py"), "-C", str(project), "-B", str(out),
                "-D", f"IDF_TARGET={info['target']}", "-D", f"SDKCONFIG={info['sdkconfig']}",
                "-D", f"SDKCONFIG_DEFAULTS={defaults}", "-D", f"XIAOTAI_BOARD_DIR={variant['board']}",
                "-D", f"DEPENDENCIES_LOCK={out / 'dependencies.lock'}"]
        request_cmake_inputs(out)
        subprocess.run(args + ["reconfigure"], check=True)
        effective = read_config(Path(info["sdkconfig"]))
        expected = info["config"] if stage == "firmware" else {
            k: v for k, v in info["config"].items() if not k.startswith(("CONFIG_XIAOTAI_", "CONFIG_BOARD_TYPE_"))}
        verify_config(effective, expected)
        subprocess.run(args + ["build"], check=True)
        info["sdkconfig_sha256"] = hashlib.sha256(Path(info["sdkconfig"]).read_bytes()).hexdigest()
        info["idf_identity"] = idf_identity
        if "git_commit" in idf_identity:
            info["idf_commit"] = idf_identity["git_commit"]
        info["artifacts"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in sorted(out.glob("*")) if p.suffix in {".elf", ".bin", ".map"}}
        paths = build_input_paths(out)
        sdk_dir = sdk_directory(variant)
        paths.update(sdk_dir.glob("sdkconfig.defaults"))
        paths.update([sdk_dir / "libTiRTC.a", sdk_dir / "contract.json",
                      ROOT / "main/boards" / variant["board"] / "config.json",
                      ROOT / "scripts/xiaotai_build.py", Path(info["sdkconfig"])])
        paths.update([ROOT / "scripts/build.py", ROOT / "config/development-status.json",
                      ROOT / "config/xiaotai.defaults", ROOT / "scripts/idf_environment.py"])
        paths.update(contract_paths(variant, ROOT))
        if stage == "firmware":
            paths.add(dependency_lock(variant))
        board_recipe = ROOT / "main/boards" / variant["board"] / "board.cmake"
        if board_recipe.is_file():
            paths.add(board_recipe)
        paths.update(out / name for name in ("build.ninja", "compile_commands.json", "CMakeCache.txt", "config/sdkconfig.h"))
        for directory in (out / "toolchain", out / "specs", ROOT / "cmake"):
            paths.update(path for path in directory.rglob("*") if path.is_file())
        paths.update(path for directory in (ROOT, ROOT / "main", ROOT / "components/tirtc_sdk", project, project / "main")
                     for path in directory.iterdir()
                     if path.is_file() and (path.name.startswith(("CMakeLists", "Kconfig", "idf_component")) or path.suffix == ".cmake"))
        manifest = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(paths)}
        manifest_path = out / "build-inputs.json"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        info["input_manifest_sha256"] = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
        info["input_file_count"] = len(manifest)
        info["sdk_sha256"] = manifest[str(sdk_dir / "libTiRTC.a")]
        if (out / "dependencies.lock").is_file():
            info["dependencies_lock_sha256"] = hashlib.sha256((out / "dependencies.lock").read_bytes()).hexdigest()
        (out / "build-record.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    finally:
        handle.close()
        lock.unlink()


def main(argv=None):
    parser = argparse.ArgumentParser(description="Build XiaoTai from one shared source tree; never flashes or publishes")
    parser.add_argument("board", nargs="?")
    parser.add_argument("--name")
    parser.add_argument("--list-boards", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--fresh-config", action="store_true")
    parser.add_argument("--stage", choices=("firmware", "sdk-check"), default="firmware")
    parser.add_argument("--build-options-json", default="{}")
    args = parser.parse_args(argv)
    try:
        if args.list_boards:
            if args.board or args.name:
                parser.error("--list-boards does not accept a board/name")
            data = variants()
            if args.json:
                print(json.dumps(data, ensure_ascii=False, indent=2))
            else:
                for item in data:
                    print(f"{item['board']:48} {item['name']:32} {item['target']:8} {item['xiaotai']['status']}")
            return
        if not args.board:
            parser.error("Select a board, or use --list-boards")
        item = select(args.board, args.name)
        options = json.loads(args.build_options_json)
        if args.dry_run:
            print(json.dumps(plan(item, args.stage, options), indent=2))
        else:
            run(item, args.stage, options, args.fresh_config)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"ERROR: {error}\n")


if __name__ == "__main__":
    main()
