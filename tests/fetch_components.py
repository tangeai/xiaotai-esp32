#!/usr/bin/env python3
"""Prepare the locked component sources that host tests compile.

Every source is checked against the product lock's component_hash, copied into a
staging directory, checked again and then moved into build/host-components (or
XIAOTAI_HOST_COMPONENTS). Local sources are tried first; the registry is used
only with --download. Exit code 2 lists what could not be prepared.
"""
import argparse
import glob
import http.client
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_components as hc  # noqa: E402

ROOT = hc.ROOT
SETS = {"default": hc.DEFAULT_SET, "all": hc.ALL_SET}
ISOLATED_VARIABLE = "XIAOTAI_HOST_COMPONENTS_ISOLATED"
FILE_STORAGE = "https://components-file.espressif.com/"
MAX_ARCHIVE_BYTES = 2 << 30
MAX_DOWNLOAD_BYTES = 512 << 20


def is_wsl():
    try:
        return "microsoft" in Path("/proc/version").read_text(encoding="ascii", errors="ignore").lower()
    except OSError:
        return False


def default_cache():
    if sys.platform.startswith("win"):
        base = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData/Local")
        return Path(base) / "Espressif/ComponentManager/Cache"
    if sys.platform == "darwin":
        return Path.home() / "Library/Caches/Espressif/ComponentManager"
    return Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "Espressif/ComponentManager"


def cache_roots(isolated):
    roots = [Path(p) for p in (os.environ.get("IDF_COMPONENT_CACHE_PATH"),) if p] + [default_cache()]
    if not isolated:
        if os.environ.get("LOCALAPPDATA"):
            roots.append(Path(os.environ["LOCALAPPDATA"]) / "Espressif/ComponentManager/Cache")
        if is_wsl():
            # Windows-side caches of the same machine, seen from WSL (any mounted drive).
            roots += [Path(p) for p in sorted(glob.glob("/mnt/?/Users/*/AppData/Local/Espressif/ComponentManager/Cache"))]
    unique = []
    for root in roots:
        if root not in unique:
            unique.append(root)
    return unique


def storage_roots(isolated):
    """ESP-IDF Installation Manager's offline component storage (official zips)."""
    if isolated:
        return []
    # The installer's tools folder: IDF_TOOLS_PATH when an environment is active, else its
    # default location on Windows, also as seen from WSL on any mounted drive.
    roots = [Path(os.environ["IDF_TOOLS_PATH"]) / "components"] if os.environ.get("IDF_TOOLS_PATH") else []
    roots += [Path("C:/Espressif/tools/components")]
    roots += [Path(p) for p in sorted(glob.glob("/mnt/?/Espressif/tools/components"))]
    return [root for index, root in enumerate(roots) if root not in roots[:index]]


def verify_source(identity, directory):
    """None when the directory holds exactly the locked component, else the reason."""
    try:
        actual = hc.component_hash(directory)
        version = hc.read_manifest(directory).get("version")
    except (hc.ComponentError, OSError, ValueError) as error:
        return str(error)
    if actual != identity.component_hash:
        return f"component_hash {actual[:12]} is not the locked {identity.component_hash[:12]}"
    if version is not None and str(version) != identity.version:
        return f"manifest version {version} is not the locked {identity.version}"
    return None


def safe_member(name):
    """Archive member name as relative parts, or ComponentError when it could escape."""
    hc.check_name(name, "archive")
    normalized = name.replace("\\", "/")
    if normalized.startswith("/") or re.match(r"^[A-Za-z]:", normalized) or ":" in normalized:
        raise hc.ComponentError(f"archive member with absolute or drive path: {name!r}")
    parts = [p for p in PurePosixPath(normalized).parts if p not in ("", ".")]
    if ".." in parts:
        raise hc.ComponentError(f"archive member leaves the extraction directory: {name!r}")
    return parts


def safe_extract(archive, destination):
    """Extract regular files and directories only; reject links, devices and escaping names."""
    destination = Path(destination)
    total = 0

    def place(name, opener, size, is_dir):
        nonlocal total
        parts = safe_member(name)
        if not parts:
            return
        target = destination.joinpath(*parts)
        if is_dir:
            target.mkdir(parents=True, exist_ok=True)
            return
        total += size
        if total > MAX_ARCHIVE_BYTES:
            raise hc.ComponentError(f"{archive} expands beyond {MAX_ARCHIVE_BYTES >> 20} MiB")
        if target.exists():
            raise hc.ComponentError(f"duplicate archive member: {name!r}")
        target.parent.mkdir(parents=True, exist_ok=True)
        with opener() as source, open(target, "xb") as output:
            shutil.copyfileobj(source, output)

    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as bundle:
            for info in bundle.infolist():
                mode = info.external_attr >> 16
                if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) and not (stat.S_ISREG(mode) or stat.S_ISDIR(mode))):
                    raise hc.ComponentError(f"archive member is not a regular file: {info.filename!r}")
                place(info.filename, lambda info=info: bundle.open(info), info.file_size, info.is_dir())
    elif tarfile.is_tarfile(archive):
        with tarfile.open(archive) as bundle:
            for member in bundle.getmembers():
                if not (member.isfile() or member.isdir()):
                    raise hc.ComponentError(f"archive member is not a regular file: {member.name!r}")
                place(member.name, lambda member=member: bundle.extractfile(member), member.size, member.isdir())
    else:
        raise hc.ComponentError(f"{archive} is not a zip or tar archive")
    # A user-made archive may wrap the component in one top-level directory.
    entries = list(destination.iterdir())
    if not (destination / "idf_component.yml").exists() and len(entries) == 1 and entries[0].is_dir():
        return entries[0]
    return destination


def install(identity, source, label, base):
    """Copy the verified file set through staging into place; keep a valid concurrent result."""
    base.mkdir(parents=True, exist_ok=True)
    target = base / identity.dirname
    staging = Path(tempfile.mkdtemp(prefix=".staging-", dir=base))
    try:
        for relative in hc.component_files(source):
            destination = staging / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(Path(source) / relative, destination)
        problem = verify_source(identity, staging)
        if problem:
            raise hc.ComponentError(f"copy from {label} changed while staging: {problem}")
        if target.exists() and hc.check_prepared(identity, base) and verify_source(identity, target):
            stale = Path(tempfile.mkdtemp(prefix=".stale-", dir=base))
            os.replace(target, stale / "old")
            shutil.rmtree(stale, ignore_errors=True)
        try:
            os.rename(staging, target)
        except OSError:
            # Another run placed it first: keep theirs if it verifies.
            problem = verify_source(identity, target) if target.is_dir() else "missing"
            if problem:
                raise hc.ComponentError(f"{target} exists but is not the locked component: {problem}")
            shutil.rmtree(staging, ignore_errors=True)
        hc.write_stamp(identity, target, label)
    finally:
        if staging.exists():
            shutil.rmtree(staging, ignore_errors=True)


def directory_candidates(identity, directories):
    prefix = f"{identity.namespace}__{identity.name}"
    for directory in directories:
        directory = Path(directory)
        if not directory.is_dir():
            yield None, f"--from {directory}: not a directory"
            continue
        yield directory, f"--from {directory}"
        for child in sorted(directory.iterdir()):
            if child.is_dir() and (child.name == prefix or child.name.startswith(prefix + "_")):
                yield child, f"--from {child}"


def cache_candidates(identity, caches):
    for cache in caches:
        for candidate in sorted(cache.glob("*/" + identity.dirname)):
            yield candidate, f"cache {candidate}"


def storage_archives(identity, storages):
    for storage in storages:
        folder = storage / identity.namespace / identity.name / identity.version
        index = storage / identity.namespace / f"{identity.name}.json"
        try:
            listed = json.loads(index.read_text(encoding="utf-8"))["versions"]
            if not any(v.get("version") == identity.version and v.get("component_hash") == identity.component_hash
                       for v in listed):
                continue
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            continue
        for archive in sorted(folder.glob("*.zip")):
            yield archive, f"EIM storage {archive}"


def registry_archive(identity, folder):
    """Download the locked version from the official registry after checking its metadata."""
    api = f"{hc.REGISTRY}/api/components/{identity.component}"
    with urllib.request.urlopen(api, timeout=60) as response:
        metadata = json.load(response)
    entry = next((v for v in metadata.get("versions", []) if v.get("version") == identity.version), None)
    if entry is None:
        raise hc.ComponentError(f"registry does not list {identity.component} {identity.version}")
    if entry.get("yanked_at"):
        raise hc.ComponentError(f"registry marks {identity.component} {identity.version} as yanked")
    if entry.get("component_hash") != identity.component_hash:
        raise hc.ComponentError("registry component_hash differs from the product lock")
    url = urllib.parse.urljoin(FILE_STORAGE, entry.get("download_url") or entry.get("url") or "")
    parsed = urllib.parse.urlparse(url)
    host = parsed.hostname or ""
    if parsed.scheme != "https" or not (host == "espressif.com" or host.endswith(".espressif.com")):
        raise hc.ComponentError(f"unexpected download location: {url}")
    archive = Path(folder) / ("download.tgz" if parsed.path.endswith((".tgz", ".tar.gz")) else "download.zip")
    size = 0
    with urllib.request.urlopen(url, timeout=120) as response, open(archive, "wb") as output:
        for block in iter(lambda: response.read(1 << 20), b""):
            size += len(block)
            if size > MAX_DOWNLOAD_BYTES:
                raise hc.ComponentError(f"download exceeds {MAX_DOWNLOAD_BYTES >> 20} MiB")
            output.write(block)
    return archive


def prepare(identities, base=None, from_dirs=(), archives=(), download=False, isolated=False,
            caches=None, storages=None, managed=None):
    """Prepare each identity. Returns ({role: dict(status, source, notes)}, general notes)."""
    base = Path(base or hc.prepared_root())
    caches = cache_roots(isolated) if caches is None else caches
    storages = storage_roots(isolated) if storages is None else storages
    if managed is None and not isolated:
        managed = ROOT / "managed_components"
    scratch = tempfile.TemporaryDirectory(prefix="xiaotai-components-")
    extracted = {}  # archive -> (component root or None, component_hash or error)
    hashes = {}  # directory -> component_hash or error, so one --from folder is hashed once
    results, general = {}, []

    def from_archive(archive):
        if archive not in extracted:
            folder = Path(tempfile.mkdtemp(dir=scratch.name))
            try:
                root = safe_extract(archive, folder)
                extracted[archive] = (root, hc.component_hash(root))
            except (hc.ComponentError, OSError, ValueError, zipfile.BadZipFile, tarfile.TarError) as error:
                extracted[archive] = (None, str(error))
        return extracted[archive]

    def directory_hash(directory):
        if directory not in hashes:
            try:
                hashes[directory] = hc.component_hash(directory)
            except (hc.ComponentError, OSError, ValueError) as error:
                hashes[directory] = f"error: {error}"
        return hashes[directory]

    def candidates(identity):
        prefix = f"{identity.namespace}__{identity.name}"
        for directory, label in directory_candidates(identity, from_dirs):
            # A --from folder named for another component is skipped silently.
            if directory is None or directory.name.startswith(prefix) \
                    or directory_hash(directory) == identity.component_hash:
                yield directory, label
        yield from cache_candidates(identity, caches)
        for archive, label in storage_archives(identity, storages):
            root, digest = from_archive(archive)
            if root is None or digest != identity.component_hash:
                reason = digest if root is None else f"component_hash {digest[:12]} is not the locked one"
                yield None, f"{label}: {reason}"
            else:
                yield root, label
        if managed is not None and (Path(managed) / prefix).is_dir():
            yield Path(managed) / prefix, "repository managed_components"
        for archive in archives:
            root, digest = from_archive(Path(archive))
            if root is not None and digest == identity.component_hash:
                yield root, f"--archive {archive}"
        if download:
            try:
                archive = registry_archive(identity, tempfile.mkdtemp(dir=scratch.name))
                root, digest = from_archive(archive)
                yield root, "registry download" if root is not None else f"registry download: {digest}"
            except (hc.ComponentError, OSError, ValueError, http.client.HTTPException) as error:
                yield None, f"registry download: {error}"

    try:
        for identity in identities:
            notes = []
            target = base / identity.dirname
            problem = hc.check_prepared(identity, base)
            if problem is None:
                results[identity.role] = dict(status="ready", source="prepared", notes=notes)
                continue
            if target.is_dir() and verify_source(identity, target) is None:
                # Content still matches; only the inventory (for example mtimes) was stale.
                hc.write_stamp(identity, target, "prepared (re-verified)")
                results[identity.role] = dict(status="ready", source="prepared (re-verified)", notes=notes)
                continue
            if target.is_dir():
                notes.append(f"prepared copy rejected: {problem}")
            done = None
            for source, label in candidates(identity):
                if source is None:
                    notes.append(f"rejected {label}")
                    continue
                problem = verify_source(identity, source)
                if problem:
                    notes.append(f"rejected {label}: {problem}")
                    continue
                try:
                    install(identity, Path(source), label, base)
                except (hc.ComponentError, OSError) as error:
                    notes.append(f"rejected {label}: {error}")
                    continue
                done = label
                break
            results[identity.role] = dict(status="ready" if done else "missing", source=done, notes=notes)
        wanted = {identity.component_hash for identity in identities}
        for archive in archives:
            root, digest = from_archive(Path(archive))
            if root is None:
                general.append(f"rejected --archive {archive}: {digest}")
            elif digest not in wanted:
                general.append(f"--archive {archive} is no locked component of this set (component_hash {digest[:12]})")
    finally:
        scratch.cleanup()
    return results, general


def _write_atomic(path, text):
    # run_host.py and CMake may read these while another run rewrites them.
    temporary = path.with_name(f".{path.name}.{os.getpid()}")
    temporary.write_text(text, encoding="utf-8")
    os.replace(temporary, path)


def _still_stamped(entry, base):
    """The earlier paths entry rebuilt for this platform, or None when its stamped copy is gone."""
    if not isinstance(entry, dict) or not all(isinstance(entry.get(k), str) for k in
                                              ("component", "version", "component_hash", "cmake", "dir")):
        return None
    # dir is an absolute path of the OS that wrote it (a Windows drive path or a WSL mount path); only its name is portable.
    name = PurePosixPath(entry["dir"]).name
    target = base / name
    if not name or not target.is_dir():
        return None
    try:
        stamp = json.loads((base / (name + hc.STAMP_SUFFIX)).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(stamp, dict) or stamp.get("component_hash") != entry["component_hash"]:
        return None
    return dict(entry, dir=target.as_posix())


def write_paths(identities, results, base=None, roles=None, root=ROOT):
    """paths.json and paths.cmake from the roles this run verified.

    Roles this run did not handle (not requested, or their lock could not be read) keep an
    earlier entry while its stamp still names the same component; the Python tests re-check
    files at use time, so this avoids re-inventorying, for example, LVGL on every run_host.
    """
    base = Path(base or hc.prepared_root())
    base.mkdir(parents=True, exist_ok=True)
    try:
        previous = json.loads((base / "paths.json").read_text(encoding="utf-8")).get("components")
    except (OSError, ValueError, AttributeError):
        previous = None
    previous = previous if isinstance(previous, dict) else {}
    handled = {identity.role: identity for identity in identities}
    entries = {}
    for role in (hc.ROLES if roles is None else roles):
        identity = handled.get(role)
        if identity is not None:
            if results[role]["status"] == "ready":
                entries[role] = dict(component=identity.component, version=identity.version,
                                     component_hash=identity.component_hash, cmake=identity.cmake_variable,
                                     dir=(base / identity.dirname).as_posix())
            continue
        old = _still_stamped(previous.get(role), base)
        if old is None:
            continue
        try:
            current = hc.resolve(role, root=root, roles=roles)
        except hc.ComponentError:
            current = None  # lock unreadable now: keep the earlier verified entry
        if current is None or current.component_hash == old["component_hash"]:
            entries[role] = old
    _write_atomic(base / "paths.json", json.dumps(dict(schema=1, components=entries), indent=2) + "\n")
    lines = ["# Generated by tests/fetch_components.py; verified host-test component sources."]
    for role, entry in entries.items():
        quoted = entry["dir"].replace('"', '\\"').replace("$", "\\$")
        lines.append(f'set({entry["cmake"]} "{quoted}")  # {entry["component"]} {entry["version"]}')
    _write_atomic(base / "paths.cmake", "\n".join(lines) + "\n")
    return entries


def identities_for(roles, line=None):
    found, errors = [], {}
    for role in roles:
        try:
            found.append(hc.resolve(role, line))
        except hc.ComponentError as error:
            errors[role] = str(error)
    return found, errors


def main(argv=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--set", choices=SETS, default="default",
                        help="default: what run_host.py needs plus cJSON for the CMake projects; all: adds LVGL")
    parser.add_argument("--role", action="append", choices=hc.ROLES, help="prepare only these roles")
    parser.add_argument("--from", dest="from_dirs", action="append", default=[], type=Path, metavar="DIR",
                        help="an extracted component, or a folder holding <ns>__<name>* copies")
    parser.add_argument("--archive", action="append", default=[], type=Path, metavar="FILE",
                        help="a component zip/tar downloaded from the registry")
    parser.add_argument("--download", action="store_true", help="allow downloading from components.espressif.com")
    parser.add_argument("--isolated", action="store_true",
                        help=f"only IDF_COMPONENT_CACHE_PATH/the default cache and explicit sources (also {ISOLATED_VARIABLE}=1)")
    parser.add_argument("--idf", metavar="LINE", help=f"ESP-IDF line of the locks (also {hc.LINE_VARIABLE})")
    args = parser.parse_args(argv)
    if args.idf:
        os.environ[hc.LINE_VARIABLE] = args.idf
    try:
        # A bad line must stop before anything, paths.* included, is touched.
        hc.selected_line()
    except hc.ComponentError as error:
        print(f"error    {error}", file=sys.stderr)
        return 1
    # Recovery hints repeat the selection the user made.
    options = "".join(f" --role {role}" for role in args.role) if args.role else \
        (f" --set {args.set}" if args.set != "default" else "")
    if os.environ.get(hc.LINE_VARIABLE):
        options += f" --idf {os.environ[hc.LINE_VARIABLE]}"
    isolated = args.isolated or os.environ.get(ISOLATED_VARIABLE) == "1"
    identities, errors = identities_for(args.role or SETS[args.set])
    results, general = prepare(identities, from_dirs=args.from_dirs, archives=args.archive,
                               download=args.download, isolated=isolated)
    write_paths(identities, results)
    base = hc.prepared_root()
    for identity in identities:
        result = results[identity.role]
        print(f"{result['status']:8} {identity.role:12} {identity.component} {identity.version}"
              + (f"  <- {result['source']}" if result["source"] else ""))
        for note in result["notes"]:
            print(f"         {note}")
    for role, error in errors.items():
        print(f"error    {role:12} {error}")
    for note in general:
        print(note)
    missing = [i for i in identities if results[i.role]["status"] != "ready"]
    if missing:
        print("\n无法准备以下组件 / could not prepare:")
        for identity in missing:
            print(hc.instructions(identity, "no verified source found", base, options))
        if not args.download:
            print(f"或允许联网下载 / or allow the registry: {hc.prepare_command(None, options)} --download")
    print(f"paths: {base / 'paths.json'}, {base / 'paths.cmake'}")
    if errors:
        return 1
    return 2 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
