"""Resolve XiaoTai product profiles and isolate their effective build inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import elf_static_memory
from flash_layout import flash_layout
from idf_environment import LOCKS as IDF_LOCKS, active_version, verify as verify_idf

ROOT = Path(__file__).resolve().parents[1]
FEATURES = ("ai", "device_call", "wechat_voip", "room", "h5_stream", "h5_video", "ai_vision")
TARGETS = {"esp32s3", "esp32p4", "esp32c3"}


def variants(root=ROOT):
    """Every product: a declaration (products/<name>/product.json) with a locked native
    profile. The board id is the profile's command-line board."""
    result = []
    for path in sorted((root / "products").glob("*/product.json")):
        name = path.parent.name
        profile = root / "profiles/native-firmware" / f"{name}.json"
        if not profile.is_file():
            continue
        data = json.loads(profile.read_text(encoding="utf-8"))
        if data["target"] not in TARGETS:
            raise ValueError(f"No TiRTC SDK for {data['target']}")
        result.append(dict(board=data.get("cli_board", data["board"]), target=data["target"], name=name,
                           entry="sdk", sdkconfig_append=[], xiaotai=dict(schema=1, profile="sdk")))
    return result


def sdk_package_name(sdk, target):
    """A product's per-line SDK choice; "generic" names the line's package for the chip."""
    return f"generic-{target}" if sdk == "generic" else sdk


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


def sdk_lines(root=ROOT):
    """The ESP-IDF lines and their TiRTC packages: components/tirtc_sdk/packages.json."""
    return json.loads((root / "components/tirtc_sdk/packages.json").read_text(encoding="utf-8"))


def resolve_line(line=None, root=ROOT):
    lines = sdk_lines(root)
    line = lines["default_line"] if line is None else line
    if line not in lines["lines"]:
        raise ValueError(f"Unknown ESP-IDF line {line}; choose one of {', '.join(lines['lines'])}")
    return line


def sdk_package(variant, line=None, root=ROOT, *, choices=None):
    """Resolve TiRTC from the selected entry's package declarations."""
    line = resolve_line(line, root)
    if choices is None:
        # A product declaration without "tirtc" uses the generic package on every line;
        # its native profiles decide which lines it actually builds on.
        product = json.loads((root / "products" / variant["name"] / "product.json").read_text(encoding="utf-8"))
        choices = product.get("tirtc") or {name: "generic" for name in sdk_lines(root)["lines"]}
    if line not in choices:
        raise ValueError(f"{variant['name']} does not support ESP-IDF {line}; supported lines: {', '.join(choices)}")
    name = sdk_package_name(choices[line], variant["target"])
    entry = sdk_lines(root)["lines"][line]["packages"].get(name)
    if entry is None:
        raise ValueError(f"{variant['name']}: no TiRTC package {name} on ESP-IDF {line}")
    sdk = root / "components/tirtc_sdk"
    if "path" in entry:
        # An official package: downloaded by scripts/fetch_tirtc.py, never stored in the tree.
        unpacked = sdk / "packages" / entry["path"].rsplit("/", 1)[-1]
        lib, include = unpacked / "lib", unpacked / "include"
    else:
        lib, include = sdk / entry["lib"], sdk / entry["include"]
    contract = sdk / entry["contract"] if "contract" in entry else lib / "contract.json"
    return {"line": line, "key": f"{line}/{name}", "lib": lib, "include": include, "entry": entry,
            "contract": contract, "defaults": lib / "sdkconfig.defaults"}


def sdk_directory(variant, root=ROOT, line=None):
    return sdk_package(variant, line, root)["lib"]


def sdk_requirements(contract, stage):
    """Settings the TiRTC archive needs, applied after every product layer.

    A "full" crypto contract is the producer's whole configuration; the board must
    already match it, so firmware only receives it in the isolated sdk-check stage.
    A "required" contract lists only what the archive depends on and always wins.
    """
    values = {}
    mode = contract.get("crypto_mode", "full")
    if mode != "bundled" and (stage == "sdk-check" or mode == "required"):
        for key, value in contract["crypto_config"].items():
            values[key] = value if re.fullmatch(r"y|n|[0-9]+|0x[0-9a-fA-F]+", value) else json.dumps(value)
    values.update(contract.get("atomic_config", {}))
    values.update(contract.get("libc_config", {}))
    return values


CAPABILITIES = {"psram": lambda board: board.get("CONFIG_SPIRAM") == "y"}


def board_capabilities(board_values):
    """Capability layers (config/capabilities/<name>.defaults) that apply to these board settings."""
    return [name for name, applies in CAPABILITIES.items() if applies(board_values)]


def resolver_reads_relative_paths():
    """Component manager 3.x (ESP-IDF 6) resolves lock paths against the lock; 2.x (ESP-IDF 5.5) cannot."""
    from importlib.metadata import PackageNotFoundError, version
    try:
        return int(version("idf-component-manager").split(".")[0]) >= 3
    except (PackageNotFoundError, ValueError):
        return True


def seed_lock(source, destination, force_solve=False):
    """Place a committed lock where the resolver reads it, in a form it can load.

    force_solve keeps the old solution as the resolver's preference but makes it
    solve again, so resolver constraints apply even when no manifest changed.
    """
    if resolver_reads_relative_paths() and not force_solve:
        shutil.copyfile(source, destination)
        return
    yaml = load_yaml()
    lock = yaml.safe_load(source.read_text(encoding="utf-8-sig"))
    if not resolver_reads_relative_paths():
        for item in local_sources(lock):
            if not Path(item["path"]).is_absolute():
                item["path"] = os.path.normpath(destination.parent / item["path"])
    if force_solve:
        lock["manifest_hash"] = "0" * 64
    destination.write_text(yaml.safe_dump(lock, sort_keys=False, allow_unicode=True), encoding="utf-8", newline="\n")


def resolver_environment():
    """Environment for every idf.py call: git components check out with LF on every OS.

    Their lock hash covers the checked-out files, so a Windows autocrlf checkout would
    otherwise give a different hash from the same commit.
    """
    environment = dict(os.environ)
    count = int(environment.get("GIT_CONFIG_COUNT", "0"))
    for offset, (key, value) in enumerate((("core.autocrlf", "false"), ("core.eol", "lf"))):
        environment[f"GIT_CONFIG_KEY_{count + offset}"] = key
        environment[f"GIT_CONFIG_VALUE_{count + offset}"] = value
    environment["GIT_CONFIG_COUNT"] = str(count + 2)
    return environment


def load_yaml():
    # IDF supplies PyYAML. Listing boards and previewing configs do not need it.
    try:
        import yaml
    except ImportError as error:
        raise ValueError("Activate the ESP-IDF Python environment to check dependency locks") from error
    return yaml


def local_sources(lock):
    for component in lock.get("dependencies", {}).values():
        source = component.get("source") if isinstance(component, dict) else None
        if isinstance(source, dict) and source.get("type") == "local" and isinstance(source.get("path"), str):
            yield source


def portable_local_paths(lock, base):
    """Rewrite local component paths relative to the effective lock's directory."""
    for source in local_sources(lock):
        path = Path(source["path"])
        if path.is_absolute():
            source["path"] = Path(os.path.relpath(path, base)).as_posix()
        else:
            source["path"] = path.as_posix()
    return lock


def write_dependency_lock(effective, destination):
    """Store a reviewed resolver result as the product lock, without machine paths."""
    yaml = load_yaml()
    lock = yaml.safe_load(effective.read_text(encoding="utf-8-sig"))
    portable_local_paths(lock, effective.parent)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(yaml.safe_dump(lock, sort_keys=False, allow_unicode=True), encoding="utf-8", newline="\n")


def read_dependency_lock(path):
    yaml = load_yaml()

    try:
        lock = yaml.safe_load(path.read_text(encoding="utf-8-sig"))
    except (yaml.YAMLError, OSError) as error:
        raise ValueError(f"Cannot read dependency lock: {path}: {error}") from error
    if (not isinstance(lock, dict) or not all(isinstance(key, str) for key in lock)
            or not isinstance(lock.get("dependencies"), dict)
            or not all(isinstance(name, str) and isinstance(component, dict)
                       for name, component in lock["dependencies"].items())
            or not isinstance(lock.get("direct_dependencies"), list)
            or not isinstance(lock.get("target"), str)
            or not isinstance(lock.get("version"), str)
            or not re.fullmatch(r"[0-9a-f]{64}", str(lock.get("manifest_hash", "")))):
        raise ValueError(f"Invalid dependency lock: {path}")
    return lock


def verify_dependency_lock(source, effective):
    def read(path):
        lock = read_dependency_lock(path)
        # The resolver may refresh its manifest fingerprint without changing
        # the resolved graph, component contents or target.
        del lock["manifest_hash"]
        return portable_local_paths(lock, Path(effective).parent)

    expected, resolved = read(source), read(effective)
    if expected == resolved:
        return
    expected_components = expected.pop("dependencies")
    resolved_components = resolved.pop("dependencies")
    components = sorted(name for name in expected_components.keys() | resolved_components.keys()
                        if expected_components.get(name) != resolved_components.get(name))
    missing = object()
    metadata = sorted(key for key in expected.keys() | resolved.keys()
                      if expected.get(key, missing) != resolved.get(key, missing))
    details = [f"component {name}" for name in components] + [f"field {key}" for key in metadata]
    raise ValueError(f"Dependency lock differs from the product contract: {', '.join(details)}; "
                     f"compare {effective} with {source}. No automatic lock update.")


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


def lock_fingerprint(path):
    match = re.search(r"^manifest_hash: *([0-9a-f]{64})", path.read_text(encoding="utf-8-sig"), re.MULTILINE)
    return match.group(1) if match else None


def process_alive(pid):
    """Whether a process with this PID exists on this machine; unsure means alive."""
    if os.name == "nt":
        import ctypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return ctypes.get_last_error() != 87  # ERROR_INVALID_PARAMETER: no such process
        try:
            code = ctypes.c_ulong()
            return not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)) or code.value == 259  # STILL_ACTIVE
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def build_owner():
    return {"pid": os.getpid(), "host": socket.gethostname(), "platform": sys.platform}


def acquire_build_lock(lock):
    """Take the build lock; a lock left by an ended build on this machine is cleared.

    Only a lock this machine and OS wrote can be judged: a WSL build and a Windows
    build share the folder but not their process tables.
    """
    for _ in range(2):
        try:
            with lock.open("x", encoding="utf-8") as handle:
                handle.write(json.dumps(build_owner()))
            return
        except FileExistsError:
            try:
                owner = json.loads(lock.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                owner = None
            mine = isinstance(owner, dict) and {key: owner.get(key) for key in ("host", "platform")} == \
                {key: build_owner()[key] for key in ("host", "platform")}
            # The launcher may die alone; its idf.py keeps cmake and ninja running in this tree.
            if not mine or not isinstance(owner.get("pid"), int) or any(
                    process_alive(pid) for pid in (owner["pid"], owner.get("tool_pid")) if isinstance(pid, int)):
                raise ValueError(f"Build already locked: {lock} (owner {owner}); wait for that build, "
                                 f"or remove the file once you are sure it and its idf.py/ninja have ended")
            # Two builds may find the same stale lock; only one of them may remove it.
            guard = lock.with_name(lock.name + ".takeover")
            busy = ValueError(f"Build already locked: {lock} (another build is taking over an ended build's lock); retry")
            try:
                guard.open("x").close()
            except FileExistsError:
                raise busy from None
            try:
                if json.loads(lock.read_text(encoding="utf-8")) == owner:
                    lock.unlink()
                    print(f"Removed a build lock left by an ended build (PID {owner['pid']}): {lock}")
            except FileNotFoundError:
                pass  # already cleared; the retry takes the lock or reports its new owner
            except (OSError, ValueError):
                raise busy from None
            finally:
                guard.unlink(missing_ok=True)
    raise ValueError(f"Build lock keeps reappearing: {lock}")


def release_build_lock(lock):
    """Remove the lock only if this process still owns it."""
    try:
        if json.loads(lock.read_text(encoding="utf-8")).get("pid") == os.getpid():
            lock.unlink()
    except (OSError, ValueError):
        pass


def run_tool(lock, args, environment):
    """Run a build tool and name it in the lock: its children outlive a killed launcher."""
    process = subprocess.Popen(args, env=environment)
    try:
        lock.write_text(json.dumps(dict(build_owner(), tool_pid=process.pid)), encoding="utf-8")
        code = process.wait()
    except BaseException:
        process.kill()
        process.wait()
        raise
    if code:
        raise subprocess.CalledProcessError(code, args)


def list_idf(root=ROOT):
    import xiaotai_build_native as native

    lines = sdk_lines(root)
    for line, spec in lines["lines"].items():
        default = " (default)" if line == lines["default_line"] else ""
        print(f"ESP-IDF {line}{default}: {', '.join(spec['idf_versions'])}; TiRTC packages: {spec['origin']}")
        print(f"  {spec['summary']}")
        for variant in variants(root):
            if not native.profile_path(root, variant["name"], line=line).is_file():
                print(f"  {variant['board']:44} not supported")
                continue
            package = sdk_package(variant, line, root)
            contract = json.loads(package["contract"].read_text(encoding="utf-8-sig"))
            print(f"  {variant['board']:44} {package['key']:28} TiRTC {contract['sdk_version']}")
    idf = os.environ.get("IDF_PATH", "")
    active = active_version(idf) if idf else None
    print(f"Active ESP-IDF: {active or 'none'}" + (f" at {idf}" if active else ""))


def main(argv=None):
    parser = argparse.ArgumentParser(description="Build XiaoTai from one shared source tree; never flashes or publishes")
    parser.add_argument("board", nargs="?")
    parser.add_argument("--name")
    parser.add_argument("--list-boards", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    # Kept so that commands written for the removed legacy entry fail with a clear message.
    parser.add_argument("--fresh-config", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--stage", choices=("firmware", "sdk-check"), default="firmware", help=argparse.SUPPRESS)
    parser.add_argument("--entry", choices=("legacy", "sdk"), help="only the SDK entry exists; kept for old commands")
    parser.add_argument("--build-options-json", default="{}", help=argparse.SUPPRESS)
    parser.add_argument("--variant", help="build one declared product variant")
    parser.add_argument("--component-index", type=Path, help="local file index for offline builds")
    parser.add_argument("--component-cache", type=Path, help="seed from a local IDF cache for offline builds")
    parser.add_argument("--idf", metavar="LINE", help="ESP-IDF line (default: the product profile's line)")
    parser.add_argument("--list-idf", action="store_true", help="show ESP-IDF lines, product support and the active ESP-IDF")
    parser.add_argument("--write-lock", action="store_true",
                        help="maintainers: store the resolved lock of a module-removing variant, then review the diff")
    parser.add_argument("--resolve-only", action="store_true",
                        help="resolve dependencies and configure without compiling")
    args = parser.parse_args(argv)
    try:
        if args.list_idf:
            if args.component_index or args.component_cache or args.variant is not None:
                parser.error("cache and variant options require a firmware build")
            list_idf()
            return
        if args.list_boards:
            if args.board or args.name or args.component_index or args.component_cache or args.variant is not None:
                parser.error("--list-boards does not accept board/name, cache or variant options")
            data = variants()
            if args.json:
                print(json.dumps(data, ensure_ascii=False, indent=2))
            else:
                for item in data:
                    print(f"{item['board']:48} {item['name']:32} {item['target']}")
            return
        if not args.board:
            parser.error("Select a board, or use --list-boards")
        item = select(args.board, args.name)
        if args.entry == "legacy":
            raise ValueError(f"{item['name']} has no legacy entry any more; build it with the SDK entry "
                             "(the legacy sources are at the commit in tests/migration_inventory.json)")
        import xiaotai_build_native
        xiaotai_build_native.dispatch(args, item, json.loads(args.build_options_json))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"ERROR: {error}\n")


if __name__ == "__main__":
    main()
