"""Explicit native ESP-IDF product entry; it never changes the legacy graph."""

from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time

import elf_static_memory
import image_trailer
import sdk_selection
import xiaotai_build as common
from idf_environment import verify as verify_idf


PATCH_OPTIONS = (
    "CONFIG_XIAOTAI_PORT_MQTT_NATIVE_OVERLAY",
    "CONFIG_XIAOTAI_PART_PANEL_SPI_DMA_OWNERSHIP_FIX",
    "CONFIG_XIAOTAI_PART_CAMERA_DVP_NATIVE",
    "CONFIG_XIAOTAI_PART_CAMERA_DVP_BENCH_DIAG",
)


def profile_path(root, product, *, line=None):
    """The product's native profile on its own line (line=None) or on one line.

    The top-level profile is the product's own line: usually the repository
    default, and the only line of a product that just a newer line supports
    (a new chip). Every other line keeps its profile in idf<line>/."""
    if not isinstance(product, str) or not re.fullmatch(r"[a-z0-9][a-z0-9-]*", product):
        raise ValueError("Invalid SDK-native product")
    base = root / "profiles/native-firmware"
    top = base / f"{product}.json"
    if not line:
        return top
    own = None
    if top.is_file():
        try:
            own = json.loads(top.read_text(encoding="utf-8")).get("idf_line")
        except (ValueError, AttributeError):
            own = None  # Not a readable profile; load_profile reports it. Keep the folder rule.
    if own is not None:
        return top if own == line else base / f"idf{line}" / f"{product}.json"
    return base / f"idf{line}" / f"{product}.json" if line_suffix(line, root) else top


def line_suffix(line, root):
    return f"-idf{line}" if line != common.sdk_lines(root)["default_line"] else ""


def dependency_lock_path(root, product, reduced_variant=None, *, line=None):
    stem = f"{product}-native" if reduced_variant is None else f"{product}--{reduced_variant}"
    base = root / "profiles/dependency-locks"
    return (base / f"idf{line}" if line and line_suffix(line, root) else base) / f"{stem}.lock"


def load_profile(product, root=common.ROOT, *, line=None):
    if line is not None:
        line = common.resolve_line(line, root)
    path = profile_path(root, product, line=line)
    if not path.is_file():
        suffix = f" on ESP-IDF {line}" if line is not None else ""
        raise ValueError(f"--entry sdk has no locked native profile for {product}{suffix}")
    profile = json.loads(path.read_text(encoding="utf-8"))
    version = profile["idf_version"]
    if (version not in common.IDF_LOCKS or version.rsplit(".", 1)[0] != profile["idf_line"] or
            (line is not None and line != profile["idf_line"])):
        raise ValueError(f"{product} does not select a pinned IDF version on its declared line")
    return profile


def resolved_lock_path(out, profile):
    # Local lock paths are relative to build/<product>/<entry>, as in the
    # archived native lock. Registry-only products keep their existing layout.
    return out / ("dependencies.lock" if profile.get("vendor_hashes") else "project/dependencies.lock")


def guard_node(path, directory):
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return
    expected = stat.S_ISDIR(metadata.st_mode) if directory else (
        stat.S_ISREG(metadata.st_mode) and metadata.st_nlink == 1)
    if not expected or getattr(path, "is_junction", lambda: False)():
        raise ValueError(f"Native output or dependency lock must not use links or special files: {path}")


def guard_path(root, path, *, directory=False):
    if not path.is_relative_to(root) or ".." in path.parts:
        raise ValueError(f"Native output or dependency lock is outside the repository: {path}")
    current = root
    for part in path.relative_to(root).parts:
        current /= part
        guard_node(current, current != path or directory)


def guard_output_tree(root, out):
    guard_path(root, out, directory=True)

    def fail(error):
        raise error

    for base, dirs, files in os.walk(out, onerror=fail) if out.exists() else ():
        for name in dirs:
            guard_node(Path(base) / name, True)
        for name in files:
            guard_node(Path(base) / name, False)


class LockInputs:
    """Check and hash one immutable set of bytes for each IDF lock."""

    def __init__(self, root, out, stack):
        self.root, self.out, self.stack, self.snapshots = root, out, stack, {}

    def capture(self, path, *, effective=False):
        if path not in self.snapshots:
            guard_path(self.root, path)
            # The effective lock's relative vendor paths use its parent.
            directory = path.parent if effective else self.out
            data = path.read_bytes()
            with tempfile.NamedTemporaryFile(dir=directory, prefix=".xiaotai-lock-", delete=False) as file:
                self.stack.callback(Path(file.name).unlink, missing_ok=True)
                file.write(data)
            self.snapshots[path] = Path(file.name), data
        return self.snapshots[path][0]

    def verify_unchanged(self):
        for path, (_, data) in self.snapshots.items():
            guard_path(self.root, path)
            if path.read_bytes() != data:
                raise ValueError(f"Dependency lock changed during the native build: {path}")

    def hashes(self):
        return {str(path): hashlib.sha256(data).hexdigest() for path, (_, data) in self.snapshots.items()}


def native_plan(args, board, options, root=common.ROOT):
    name = board["name"]
    requested_line = getattr(args, "idf", None)
    profile = load_profile(name, root, line=requested_line)
    line = common.resolve_line(profile["idf_line"] if requested_line is None else requested_line, root)
    board_id, target, version = profile["board"], profile["target"], profile["idf_version"]
    if (board["board"], board["target"], line, args.stage) != (
            profile.get("cli_board", board_id), target, profile["idf_line"], "firmware"):
        raise ValueError(f"--entry sdk requires the full {name} firmware on IDF {profile['idf_line']}")
    if options != {} or args.fresh_config:
        raise ValueError("--entry sdk accepts declared variants, not arbitrary build overrides")
    if (args.component_index is None) != (args.component_cache is None):
        raise ValueError("--component-index and --component-cache must be supplied together for offline builds")
    product = sdk_selection.load_product(root / "products" / name / "product.json")
    variant_name = getattr(args, "variant", None)
    reduced = False
    if variant_name is not None:
        product = sdk_selection.apply_variant(product, variant_name)
        reduced = bool(product["variants"][variant_name].get("without"))
    if args.write_lock and not (reduced and args.resolve_only and args.component_index is not None):
        raise ValueError("Native --write-lock requires a module-removing --variant, --resolve-only and local index/cache")
    product_lock = dependency_lock_path(root, name, line=line)
    pinned = dependency_lock_path(root, name, variant_name, line=line) if reduced else product_lock
    if variant_name is not None and not product_lock.is_file():
        raise ValueError(f"Missing committed SDK product lock: {product_lock}; "
                         "restore its reviewed repository copy and prepare its local index/cache before building")
    if reduced and not args.write_lock and not pinned.is_file():
        raise ValueError(f"Missing locked native variant: {pinned}; restore it or use "
                         "--write-lock --resolve-only with --component-index and --component-cache "
                         f"in an offline namespace (seed lock: {product_lock}); no online fallback")
    selection = sdk_selection.plan(root, product)
    selection.pop("units", None)
    if (selection["board"], selection["product"]) != (board_id, name):
        raise ValueError("Native product and board manifests do not match the supported entry")
    if selection["target"] != target:
        raise ValueError(f"{name}: selected board target {selection['target']} does not match locked target {target}")
    package = common.sdk_package(board, line, root, choices=product.get("tirtc"))
    contract = json.loads(package["contract"].read_text(encoding="utf-8-sig"))
    if (package["key"], contract["target"], contract["idf_version"]) != (profile["tirtc_package"], target, version):
        raise ValueError(f"{name}: TiRTC package, chip or IDF does not match the locked native profile")
    entry = ("sdk-lock" if args.write_lock else "sdk-firmware") + line_suffix(line, root)
    build = root / "build" / name / (f"{entry}--{variant_name}" if variant_name is not None else entry)
    active_target = os.environ.get("IDF_TARGET")
    if active_target and active_target != target:
        raise ValueError(f"IDF_TARGET={active_target} conflicts with {target}")
    variant_inputs = {} if variant_name is None else {"variant": variant_name}
    if reduced:
        variant_inputs["product_dependency_lock"] = product_lock.relative_to(root).as_posix()
    config = defaults(root, profile, selection, package, contract)
    verify_diagnostics_console(product, variant_name, selection, config)
    return product, package, contract, dict(
        entry="sdk", name=name, board=board_id, target=target, stage="firmware", idf_line=line,
        idf_environment={"version": version, **common.IDF_LOCKS[version]},
        build_dir=str(build), sdkconfig=str(build / "sdkconfig"), selection=selection,
        dependency_lock=pinned.relative_to(root).as_posix(),
        tirtc_package=package["key"], hardware_tested=False,
        config=config,
        **variant_inputs,
    )


def native_capability_layers(root, board_dir, board_values):
    directory = root / "config/capabilities"
    layers = [directory / f"{capability}.defaults" for capability in common.board_capabilities(board_values)]
    manifest = json.loads((board_dir / "board.json").read_text(encoding="utf-8"))
    if manifest.get("memory", {}).get("external_ram") is False:
        layers.append(directory / "no-external-ram.defaults")
    return layers


def defaults(root, profile, selection, package, contract):
    values = common.read_config(root / "config/xiaotai.defaults")
    board_dir = root / "boards" / profile["board"]
    board_values = common.read_config(board_dir / "sdkconfig.board")
    for layer in native_capability_layers(root, board_dir, board_values):
        values.update(common.read_config(layer))
    values.pop("CONFIG_XIAOTAI_AFE_MEMORY_PREFER_PSRAM", None)  # legacy AFE component only
    if package["defaults"].is_file():
        values.update(common.read_config(package["defaults"]))
    values.update(board_values)
    for layer in profile.get("defaults_layers", ()):
        values.update(common.read_config(root / layer))
    requirements = common.sdk_requirements(contract, "firmware")
    values.update(requirements)
    values.update(line.split("=", 1) for line in selection["options"] + selection["sdkconfig"])
    values.update(CONFIG_IDF_TARGET=json.dumps(profile["target"]), CONFIG_PARTITION_TABLE_CUSTOM="y",
                  CONFIG_PARTITION_TABLE_CUSTOM_FILENAME='"partitions.csv"',
                  CONFIG_XIAOTAI_PORT_HTTP_DEADLINE_TRANSPORT="y",
                  CONFIG_ESP_HTTP_CLIENT_EVENT_POST_TIMEOUT="0")
    values.update(profile.get("sdkconfig", {}))
    if package["line"] == "5.5":
        # The official archives can require crypto providers that the default
        # product graph disables for its IDF 6 SDK.
        values.update(requirements)
    return values


def verify_diagnostics_console(product, variant, selection, config, *, effective=False):
    variant_at = product.get("variants", {}).get(variant, {}).get("parameters", {}).get("at", {})
    if variant_at.get("no_platform_console") is not False:
        return
    label = f"{product['name']} variant {variant}"
    if "xiaotai_at" not in selection["components"]:
        raise ValueError(f"{label} requests physical AT but does not select the AT engine")
    # Match sdk/xiaotai_port/esp-idf/console.c: the ESP-IDF USB CDC and
    # secondary log consoles do not implement its physical AT read path.
    supported = ("USB_SERIAL_JTAG", "UART_DEFAULT", "UART_CUSTOM")
    primary = {mode: config[key] for mode in (*supported, "NONE", "USB_CDC")
               if (key := f"CONFIG_ESP_CONSOLE_{mode}") in config}
    enabled = [mode for mode, value in primary.items() if value == "y"]
    if not enabled and not effective and any(mode not in primary for mode in supported):
        return  # Kconfig can supply an unstated supported primary.
    if len(enabled) != 1 or enabled[0] not in supported:
        modes = ", ".join(enabled) or "none enabled"
        raise ValueError(f"{label} requires physical AT on one primary USB Serial/JTAG or UART backend "
                         f"(found {modes})")


def lock_dependencies(path):
    return common.read_dependency_lock(path)["dependencies"]


def verify_variant_lock(product_lock, variant_lock):
    product = common.read_dependency_lock(product_lock)
    variant = common.read_dependency_lock(variant_lock)
    if any(product[key] != variant[key] for key in ("target", "version")):
        raise ValueError("Native variant lock target or schema differs from the product lock")
    allowed = product["dependencies"]
    if "idf" not in allowed or variant["dependencies"].get("idf") != allowed["idf"]:
        raise ValueError("Native variant lock does not retain the pinned IDF")
    for name, item in variant["dependencies"].items():
        pin = allowed.get(name)
        source = item.get("source", {})
        kind = source.get("type") if isinstance(source, dict) else None
        pin_source = pin.get("source") if pin else None
        if not isinstance(pin_source, dict) or kind not in {"service", "idf", "local"} or pin_source.get("type") != kind:
            raise ValueError(f"Component {name} is outside the native product lock")
        if kind == "service":
            digest = item.get("component_hash")
            registry, pin_registry = source.get("registry_url"), pin_source.get("registry_url")
            # IDF can omit the final slash when a retained pin becomes transitive.
            if (not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest)
                    or (item.get("version"), digest) != (pin.get("version"), pin.get("component_hash"))
                    or not all(isinstance(url, str) and url for url in (registry, pin_registry))
                    or registry.removesuffix("/") != pin_registry.removesuffix("/")):
                raise ValueError(f"Component {name} changes the native product lock version, hash or source")
        elif item.get("version") != pin.get("version"):
            raise ValueError(f"Component {name} changes the native product lock version")


def save_variant_lock(resolved, pinned):
    with tempfile.TemporaryDirectory(prefix=".native-variant-", dir=pinned.parent) as scratch:
        destination = Path(scratch) / pinned.name
        common.write_dependency_lock(resolved, destination)
        destination.replace(pinned)


def component_hash_matches(path, digest):
    from idf_component_tools.hash_tools.errors import HashNotEqualError
    from idf_component_tools.hash_tools.validate import validate_hash_eq_hashdir

    try:
        validate_hash_eq_hashdir(path, digest)
    except HashNotEqualError as error:
        raise ValueError(f"Component {path} does not match {digest}: {error}") from error


def local_dependencies(profile, lock, root):
    pinned = profile.get("vendor_hashes", {})
    entries = {name: item["source"]["path"] for name, item in lock_dependencies(lock).items()
               if item["source"]["type"] == "local"}
    if entries.keys() != pinned.keys():
        raise ValueError("Local component names differ from the native profile")
    paths = {}
    vendor = (root / "vendor").resolve()
    for name, relative in entries.items():
        path = (lock.parent / relative).resolve()
        if not path.is_relative_to(vendor) or not path.is_dir():
            raise ValueError(f"Invalid locked vendor path for {name}: {path}")
        component_hash_matches(path, pinned[name])
        paths[name] = path
    return paths


def write_component_manifest(source, destination, local):
    if not local:
        shutil.copyfile(source, destination)
        return
    yaml = common.load_yaml()
    manifest = yaml.safe_load(source.read_text(encoding="utf-8"))
    for name, item in manifest["dependencies"].items():
        if name in local:
            item["override_path"] = str(local[name])
    destination.write_text(yaml.safe_dump(manifest, sort_keys=False), encoding="utf-8", newline="\n")


def stage_project_assets(root, board_id, profile, project):
    """Stage board-owned sensor JSON for components resolving from PROJECT_SOURCE_DIR."""
    board = (root / "boards" / board_id).resolve()
    project_root = project.resolve()
    inputs = set()
    for filename, location in profile.get("project_assets", {}).items():
        relative, origin = Path(filename), Path(location)
        source, target = (root / origin).resolve(), project / relative
        if (relative.is_absolute() or origin.is_absolute() or ".." in relative.parts
                or relative.suffix != ".json" or (relative.name != filename and relative != origin)
                or not source.is_relative_to(board) or not source.is_file() or target.is_symlink()
                or not target.resolve().is_relative_to(project_root)):
            raise ValueError(f"Invalid project JSON asset for {board_id}: {filename}")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        inputs.update((source, target))
    return inputs


def seed_cache(source, output, lock):
    dependencies = lock_dependencies(lock)
    entries = []
    missing = []
    for name, item in sorted(dependencies.items()):
        kind = item["source"]["type"]
        if kind in {"idf", "local"}:
            continue
        if kind != "service":
            raise ValueError(f"Native lock cannot use registry cache for {name} ({kind})")
        version, digest = item["version"], item["component_hash"]
        dirname = f"{name.replace('/', '__')}_{version}_{digest[:8]}"
        matches = [path for path in source.glob(f"service_*/{dirname}") if path.is_dir()]
        if len(matches) != 1:
            missing.append(f"{name}@{version} hash={digest} source={item['source']['registry_url']} "
                           f"expected={source}/service_*/{dirname}")
        else:
            entries.append((matches[0], digest))
    if missing:
        raise ValueError("Incomplete native IDF component cache:\n" + "\n".join(missing))
    for path, digest in entries:
        cached = output / path.parent.name / path.name
        if not cached.is_dir():
            shutil.copytree(path, cached)
        component_hash_matches(cached, digest)


def build(args, product, package, contract, info, root=common.ROOT):
    root = root.resolve()
    name, board_id = info["name"], info["board"]
    line = info["idf_line"]
    profile = load_profile(name, root, line=line)
    offline = args.component_index is not None
    if offline and (not args.component_index.is_dir() or not args.component_cache.is_dir()):
        raise ValueError("Offline component index and cache must be local directories")
    # A custom file index alone cannot prevent resolver fallback.
    if offline and (not sys.platform.startswith("linux") or [
            line.split(":", 1)[0].strip() for line in Path("/proc/net/dev").read_text().splitlines()[2:]
    ] != ["lo"]):
        raise ValueError("Run explicit offline builds inside a Linux network namespace with only loopback (unshare -Urn)")
    idf = Path(os.environ.get("IDF_PATH", "")).resolve()
    info["idf_identity"] = verify_idf(idf, profile["idf_version"])
    if line == "5.5":
        from importlib.metadata import version
        from packaging.version import Version

        manager = version("idf-component-manager")
        if profile.get("vendor_hashes") and not Version("2.5.2") <= Version(manager) < Version("3"):
            raise ValueError("ESP-IDF 5.5 native builds with local components require "
                             f"IDF Component Manager >=2.5.2,<3 (found {manager})")
        info["idf_component_manager"] = manager
    import fetch_tirtc
    if package.get("entry"):
        try:
            fetch_tirtc.ensure(package["entry"], root)
        except fetch_tirtc.FetchError as error:
            raise ValueError(str(error)) from error
    archive = package["lib"] / "libTiRTC.a"
    with archive.open("rb") as stream:
        info["sdk_sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
    if info["sdk_sha256"] != contract["sha256"]:
        raise ValueError("TiRTC archive differs from the pinned package hash")
    out = Path(info["build_dir"])
    pinned = root / info["dependency_lock"]
    product_lock = root / info.get("product_dependency_lock", info["dependency_lock"])
    updating = args.write_lock
    if updating and pinned == product_lock:
        raise ValueError("Native lock maintenance may only publish a distinct variant lock")
    if info.get("variant") is not None or updating:
        guard_output_tree(root, out)
    else:
        guard_path(root, out, directory=True)
    project = out / "project"
    resolved = resolved_lock_path(out, profile)
    guard_path(root, resolved)
    guard_path(out, Path(info["sdkconfig"]))
    out.mkdir(parents=True, exist_ok=True)
    lock = out / ".xiaotai-build.lock"
    guard_path(root, lock)
    common.acquire_build_lock(lock)
    snapshots = ExitStack()
    try:
        common.archive_build_record(out)
        pins = LockInputs(root, out, snapshots)
        product_pin = pins.capture(product_lock)
        guard_path(root, pinned)
        build_pin = product_pin if updating else pins.capture(pinned)
        if product_lock != pinned and not updating:
            verify_variant_lock(product_pin, build_pin)
        scaffold = root / "cmake" / "esp-idf-product"
        manifest = root / "profiles" / f"{name}{line_suffix(line, root)}-native.yml"
        board_dir = root / "boards" / board_id
        shutil.copytree(scaffold, project, dirs_exist_ok=True)
        assets = stage_project_assets(root, board_id, profile, project)
        shutil.copyfile(board_dir / "partitions.csv", project / "partitions.csv")
        generated = out / "generated"
        selection = sdk_selection.write(root, product, generated)
        (generated / "selection.json").write_text(json.dumps(selection, indent=2) + "\n", encoding="utf-8")
        guard_path(root, resolved)
        if updating:
            common.seed_lock(build_pin, resolved, force_solve=True)
        elif not resolved.is_file():
            common.seed_lock(build_pin, resolved)
        common.verify_dependency_lock(build_pin, resolved)
        local = local_dependencies(profile, resolved, root)
        write_component_manifest(manifest, project / "main" / "idf_component.yml", local)
        if offline:
            seed_cache(args.component_cache.resolve(), out / "component-cache", build_pin)
        config = defaults(root, profile, selection, package, contract)
        requested = out / "sdkconfig.defaults"
        expected = "".join(f"{key}={value}\n" for key, value in sorted(config.items()))
        if requested.exists() and requested.read_text(encoding="utf-8") != expected:
            raise ValueError("Native build inputs changed; use a new native output directory")
        requested.write_text(expected, encoding="utf-8", newline="\n")
        environment = common.resolver_environment()
        environment.update(IDF_PATH=str(idf), ESP_IDF_VERSION=info["idf_line"])
        if offline:
            for key in ("IDF_COMPONENT_LOCAL_STORAGE_URL", "IDF_COMPONENT_CONSTRAINTS"):
                environment.pop(key, None)
            environment.update(IDF_COMPONENT_STORAGE_URL=args.component_index.resolve().as_uri(),
                               IDF_COMPONENT_CACHE_PATH=str(out / "component-cache"))
        # CMake reads these values as strings, where a Windows backslash is an
        # escape ("\U..."): pass every path with forward slashes.
        command = [sys.executable, str(idf / "tools/idf.py"), "-C", str(project), "-B", str(out),
                   "-D", f"IDF_TARGET={info['target']}", "-D", f"SDKCONFIG={Path(info['sdkconfig']).as_posix()}",
                   "-D", f"SDKCONFIG_DEFAULTS={Path(requested).as_posix()}",
                   "-D", f"XIAOTAI_REPO={Path(root).as_posix()}",
                   "-D", f"XIAOTAI_GENERATED_DIR={Path(generated).as_posix()}",
                   "-D", f"XIAOTAI_TIRTC_PACKAGE={package['key']}",
                   "-D", f"DEPENDENCIES_LOCK={Path(resolved).as_posix()}"]
        if os.name == "nt":
            # The P4 products pass over 32 767 characters of include paths, the
            # Windows command-line limit: let Ninja hand them over in a file.
            command += ["-D", "CMAKE_NINJA_FORCE_RESPONSE_FILE=ON"]
        common.request_cmake_inputs(out)
        common.run_tool(lock, command + ["reconfigure"], environment)
        guard_path(root, resolved)
        if updating:
            effective_pin = pins.capture(resolved, effective=True)
            verify_variant_lock(product_pin, effective_pin)
            if local:
                local_dependencies(profile, effective_pin, root)
        else:
            common.verify_dependency_lock(build_pin, resolved)
        pins.verify_unchanged()
        effective = common.read_config(Path(info["sdkconfig"]))
        verify_diagnostics_console(product, info.get("variant"), selection, effective, effective=True)
        common.verify_config(effective, {**config, **dict.fromkeys(PATCH_OPTIONS, "n")})
        base = json.loads((out / "project_description.json").read_text(encoding="utf-8"))
        compiler = subprocess.check_output([base["c_compiler"], "--version"], text=True, env=environment)
        if contract["toolchain_release"] not in compiler:
            raise ValueError("Selected toolchain does not match the TiRTC producer")
        if args.resolve_only:
            if updating:
                guard_path(root, pinned)
                pins.verify_unchanged()
                save_variant_lock(effective_pin, pinned)
                print(f"Wrote offline native variant dependency lock: {pinned}")
            print(f"Native {name} configuration matches the locked profile; not compiled")
            return
        common.run_tool(lock, command + ["build"], environment)
        effective_pin = pins.capture(resolved, effective=True)
        common.verify_dependency_lock(build_pin, effective_pin)
        if local:
            local_dependencies(profile, effective_pin, root)
        app_name = info["name"].replace("-", "_") + ".bin"
        info["flash_layout"] = common.flash_layout(out, info["target"], app_name=app_name)
        if line == "5.5":
            # Keep IDF's product-named binary and flash arguments as generated.
            alias = out / "xiaotai.bin"
            guard_path(out, alias)
            shutil.copyfile(out / app_name, alias)
        trailer = out / "image_trailer.json"
        if trailer.is_file():
            # Data a unit carries behind the image, in the same file.
            info["image_trailer"] = json.loads(trailer.read_text(encoding="utf-8"))
            image_trailer.verify((out / app_name).read_bytes(), info["image_trailer"])
        info["artifacts"] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                             for path in sorted(out.glob("*")) if path.suffix in {".bin", ".elf", ".map"}}
        paths = common.build_input_paths(out)
        paths.update((Path(info["sdkconfig"]), requested, archive, package["contract"], pinned, resolved, product_lock,
                      root / "products" / name / "product.json",
                      board_dir / "board.json", profile_path(root, name, line=line),
                      manifest, root / "config/xiaotai.defaults", board_dir / "sdkconfig.board"))
        paths.update(root / layer for layer in profile.get("defaults_layers", ()))
        paths.update(native_capability_layers(root, board_dir, common.read_config(board_dir / "sdkconfig.board")))
        paths.update(assets)
        paths.update(path for base in local.values() for path in base.rglob("*") if path.is_file())
        if package["defaults"].is_file():
            paths.add(package["defaults"])
        paths.update(root / "scripts" / file for file in (
            "build.py", "xiaotai_build.py", "xiaotai_build_native.py", "sdk_selection.py",
            "idf_environment.py", "flash_layout.py", "elf_static_memory.py", "image_trailer.py"))
        paths.update(generated.iterdir())
        paths.update(out / name for name in ("build.ninja", "compile_commands.json", "CMakeCache.txt",
                                            "config/sdkconfig.h", "toolchain/cflags"))
        lock_hashes = pins.hashes()
        manifest = {str(path): lock_hashes[str(path)] if str(path) in lock_hashes
                    else hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(paths)}
        pins.verify_unchanged()
        inputs = out / "build-inputs.json"
        inputs.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        info["input_manifest_sha256"] = hashlib.sha256(inputs.read_bytes()).hexdigest()
        info["input_file_count"] = len(manifest)
        info["sdkconfig_sha256"] = manifest[info["sdkconfig"]]
        info["dependencies_lock_sha256"] = manifest[str(resolved)]
        info["static_memory"] = elf_static_memory.summarize(
            out, info, common.read_config(Path(info["sdkconfig"])), completed_at_ns=time.time_ns())
        pins.verify_unchanged()
        (out / "build-record.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
        elf_static_memory.print_notices(info["static_memory"])
    finally:
        try:
            snapshots.close()
        finally:
            lock.unlink()


def dispatch(args, board, options):
    product, package, contract, info = native_plan(args, board, options)
    if args.idf is None and line_suffix(info["idf_line"], common.ROOT):
        print(f"NOTE: {board['name']} SDK entry selects ESP-IDF {info['idf_line']} (locked product profile).",
              file=sys.stderr)
    if args.dry_run:
        print(json.dumps(info, indent=2))
    else:
        build(args, product, package, contract, info)
