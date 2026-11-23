"""Locked third-party sources for host tests.

Host tests compile real component sources (cJSON, ESP-DSP kernels, adapter inputs).
Versions and hashes come from the product dependency locks; tests/fetch_components.py
copies verified sources into build/host-components (or XIAOTAI_HOST_COMPONENTS), and
tests call path(role) at run time, never at import time, because tests import helpers
from each other.
"""
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = "https://components.espressif.com"
STAMP_SUFFIX = ".prepared.json"
# The component manager leaves these out of component_hash.
HASH_EXCLUDE = ("**/.component_hash", "**/CHECKSUMS.json")

ALL_PRODUCTS = ("xiaotai-cores3-lite", "xiaotai-szpi-s3",
                "xiaotai-waveshare-p4-rev3", "xiaotai-wt99p461-s1-1v1", "xiaotai-xmini-c3-v3")
DISPLAY_PRODUCTS = ("xiaotai-cores3-lite", "xiaotai-szpi-s3",
                    "xiaotai-waveshare-p4-rev3", "xiaotai-wt99p461-s1-1v1")

# role -> (component, products whose locks must agree on one version).
# If those products ever lock different versions, split the role per product.
ROLES = {
    "cjson": ("espressif/cjson", ALL_PRODUCTS),
    "esp-dsp": ("espressif/esp-dsp", DISPLAY_PRODUCTS),
    "hosted": ("espressif/esp_hosted", ("xiaotai-wt99p461-s1-1v1",)),
    # Every product runs the same esp-mqtt; the PSRAM policy test reads its allocation sites.
    "mqtt": ("espressif/mqtt", ALL_PRODUCTS),
    "lvgl": ("lvgl/lvgl", DISPLAY_PRODUCTS),
}
# run_host.py needs DEFAULT_SET; tests/lvgl additionally needs lvgl.
DEFAULT_SET = ("cjson", "esp-dsp", "hosted", "mqtt")
ALL_SET = DEFAULT_SET + ("lvgl",)
# Roles read from legacy firmware locks only (none since the colour products left
# the legacy entry; Xmini's legacy lock is the fallback when it has no SDK lock).
LEGACY_ROLES = set()
LINE_VARIABLE = "XIAOTAI_HOST_IDF_LINE"
# Set by run_host.py: {prepared dir: sha256 of its stamp} it verified just before the tests.
TRUSTED_VARIABLE = "XIAOTAI_HOST_COMPONENTS_TRUSTED"


class ComponentError(Exception):
    pass


@dataclass(frozen=True)
class Identity:
    role: str
    component: str
    version: str
    component_hash: str

    @property
    def namespace(self):
        return self.component.split("/", 1)[0]

    @property
    def name(self):
        return self.component.split("/", 1)[1]

    @property
    def dirname(self):
        return f"{self.namespace}__{self.name}_{self.version}_{self.component_hash[:8]}"

    @property
    def registry_page(self):
        return f"{REGISTRY}/components/{self.component}/versions/{self.version}"

    @property
    def cmake_variable(self):
        return "XIAOTAI_HOST_" + self.role.upper().replace("-", "_") + "_DIR"


def load_yaml(text, origin):
    try:
        import yaml
    except ImportError as error:
        raise ComponentError("PyYAML is required (pip install pyyaml, or use the ESP-IDF Python)") from error
    try:
        return yaml.safe_load(text)
    except yaml.YAMLError as error:
        raise ComponentError(f"cannot parse {origin}: {error}") from error


def _build_tools():
    scripts = str(ROOT / "scripts")
    if scripts not in sys.path:
        sys.path.append(scripts)
    import xiaotai_build
    return xiaotai_build


_LOCKS = {}


def _lock_entry(lock, component):
    if lock not in _LOCKS:
        data = load_yaml(lock.read_text(encoding="utf-8-sig"), lock)
        _LOCKS[lock] = data.get("dependencies", {}) if isinstance(data, dict) else {}
    entry = _LOCKS[lock].get(component)
    if not isinstance(entry, dict) or not entry.get("version") or not entry.get("component_hash"):
        raise ComponentError(f"{lock} does not lock {component} from the registry")
    return str(entry["version"]), str(entry["component_hash"])


def selected_line(line=None):
    tools = _build_tools()
    try:
        return tools.resolve_line(line or os.environ.get(LINE_VARIABLE) or None)
    except ValueError as error:
        raise ComponentError(str(error)) from error


def _product_lock(tools, product, root, line, default, *, native_allowed):
    import xiaotai_build_native as native

    # The repository defines available lines; root relocates the test fixtures.
    # Products without a lock on this line still need their default fixture sources.
    for candidate in (line, default):
        profile = native.profile_path(ROOT, product, line=candidate).relative_to(ROOT)
        if (root / profile).is_file():
            return root / native.dependency_lock_path(ROOT, product, line=candidate).relative_to(ROOT)
    return root / native.dependency_lock_path(ROOT, product, line=default).relative_to(ROOT)


def resolve(role, line=None, root=ROOT, roles=None):
    """Identity of a role from the product locks; products must agree."""
    roles = ROLES if roles is None else roles
    if role not in roles:
        raise ComponentError(f"unknown host component role {role}; known: {', '.join(roles)}")
    tools = _build_tools()
    component, products = roles[role]
    _build_tools()
    import product_scope
    # An optional product this tree leaves out has no lock to agree with.
    products = [name for name in products if name not in product_scope.OPTIONAL or product_scope.has_product(name, root)]
    if not products:
        raise ComponentError(f"role {role}: none of its products is in this tree")
    line = selected_line(line)
    default = tools.sdk_lines()["default_line"]
    found = {}
    for product in products:
        lock = _product_lock(tools, product, root, line, default, native_allowed=role not in LEGACY_ROLES)
        if not lock.is_file():
            raise ComponentError(f"missing product dependency lock: {lock}")
        found.setdefault(_lock_entry(lock, component), []).append(product)
    if len(found) != 1:
        detail = "; ".join(f"{', '.join(p)}: {v} ({h[:12]})" for (v, h), p in sorted(found.items()))
        raise ComponentError(f"role {role} covers products that lock different {component} versions "
                             f"({detail}); split the role per product in tests/host_components.py ROLES")
    (version, digest), = found
    return Identity(role, component, version, digest)


def prepared_root():
    configured = os.environ.get("XIAOTAI_HOST_COMPONENTS")
    return Path(configured).resolve() if configured else ROOT / "build/host-components"


def _sha256_file(path):
    sha = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            sha.update(block)
    return sha.hexdigest()


def check_name(name, origin):
    """component_hash encodes paths as UTF-8; a name that cannot be encoded is refused."""
    try:
        name.encode("utf-8")
    except UnicodeError:
        raise ComponentError(f"file name is not valid UTF-8 in {origin}: {name!r}") from None


def _walk_files(directory):
    """Every regular file as a posix relative path; links are refused, not followed."""
    result = []
    for base, dirs, files in os.walk(directory, followlinks=False):
        for name in dirs + files:
            full = os.path.join(base, name)
            check_name(name, base)
            if os.path.islink(full):
                raise ComponentError(f"symbolic link in component source: {full}")
        for name in files:
            full = os.path.join(base, name)
            if not os.path.isfile(full):
                raise ComponentError(f"not a regular file: {full}")
            result.append(Path(os.path.relpath(full, directory)).as_posix())
    return result


def read_manifest(directory):
    manifest = Path(directory) / "idf_component.yml"
    if not manifest.is_file():
        return {}
    data = load_yaml(manifest.read_text(encoding="utf-8-sig"), manifest)
    return data if isinstance(data, dict) else {}


def component_files(directory):
    """The files component_hash covers, in hashing order (idf-component-manager rules)."""
    directory = Path(directory)
    files = (read_manifest(directory).get("files") or {})
    if not isinstance(files, dict):
        raise ComponentError(f"unsupported files section in {directory / 'idf_component.yml'}")
    if files.get("include") or files.get("use_gitignore"):
        # Their selection depends on git state or include order; refuse rather than guess.
        raise ComponentError(f"{directory / 'idf_component.yml'} uses files.include or use_gitignore; "
                             "not supported for host components")
    patterns = files.get("exclude") or []
    if not isinstance(patterns, list) or not all(isinstance(p, str) for p in patterns):
        raise ComponentError(f"files.exclude must be a list of patterns in {directory / 'idf_component.yml'}")
    # Windows paths compare case-insensitively, as the component manager's Path sets do.
    key = str.lower if os.name == "nt" else str
    selected = {key(p): p for p in _walk_files(directory)}
    for pattern in (*patterns, *HASH_EXCLUDE):
        try:
            matches = list(directory.glob(pattern))
        except (ValueError, NotImplementedError) as error:
            raise ComponentError(f"unsupported exclude pattern {pattern!r}: {error}") from error
        for match in matches:
            selected.pop(key(match.relative_to(directory).as_posix()), None)
    return sorted(selected.values())


def component_hash(directory):
    """sha256 over (relative path, sha256 hex of content) per file, as the registry computes it."""
    sha = hashlib.sha256()
    for relative in component_files(directory):
        sha.update(relative.encode("utf-8"))
        sha.update(_sha256_file(Path(directory) / relative).encode("utf-8"))
    return sha.hexdigest()


def inventory(directory):
    entries = []
    for relative in sorted(_walk_files(directory)):
        info = os.stat(Path(directory) / relative)
        entries.append([relative, info.st_size, info.st_mtime_ns])
    return entries


def stamp_path(identity, base=None):
    return (base or prepared_root()) / (identity.dirname + STAMP_SUFFIX)


def write_stamp(identity, target, source):
    """Record a fully verified directory so later use-time checks only stat files."""
    stamp = stamp_path(identity, target.parent)
    data = {"schema": 1, "component": identity.component, "version": identity.version,
            "component_hash": identity.component_hash, "source": source, "files": inventory(target)}
    temporary = stamp.with_name(f".{stamp.name}.{os.getpid()}")
    temporary.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")
    os.replace(temporary, stamp)


def stamp_digest(identity, base=None):
    try:
        return hashlib.sha256(stamp_path(identity, base).read_bytes()).hexdigest()
    except OSError:
        return None


def check_prepared(identity, base=None):
    """None when the prepared directory still matches its verified inventory, else the reason."""
    base = base or prepared_root()
    target = base / identity.dirname
    if not target.is_dir():
        return "not prepared"
    try:
        stamp = json.loads(stamp_path(identity, base).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return "no verification record"
    expected = (identity.component, identity.version, identity.component_hash)
    if (stamp.get("component"), stamp.get("version"), stamp.get("component_hash")) != expected:
        return "verification record is for another component"
    try:
        if inventory(target) != stamp.get("files"):
            return "files changed since preparation"
    except (ComponentError, OSError) as error:
        return f"cannot read prepared files: {error}"
    return None


def prepare_command(identity=None, options=""):
    """The fetch command line; options are what the user already passed (--set/--role/--idf)."""
    if not options:
        options = " --set all" if identity is not None and identity.role not in DEFAULT_SET else ""
        line = os.environ.get(LINE_VARIABLE)
        options += f" --idf {line}" if line else ""
    return "python3 tests/fetch_components.py" + options


def instructions(identity, problem, base=None, options=""):
    base = base or prepared_root()
    command = prepare_command(identity, options)
    return "\n".join([
        f"主机测试组件不可用 / host component unavailable: {identity.component} {identity.version} "
        f"(role {identity.role}): {problem}",
        f"  准备 / prepare:          {command}",
        f"  手动下载 / download:     {identity.registry_page}",
        f"  导入 / import:           {command} --archive <下载的 zip / downloaded zip>",
        f"  component_hash:          {identity.component_hash}",
        f"  目标目录 / target:       {base / identity.dirname}",
    ])


def _trusted(identity, target):
    """True when run_host.py verified this directory and its stamp has not changed since."""
    try:
        trusted = json.loads(os.environ.get(TRUSTED_VARIABLE) or "{}")
    except ValueError:
        return False
    expected = trusted.get(target.as_posix()) if isinstance(trusted, dict) else None
    return bool(expected) and target.is_dir() and stamp_digest(identity, target.parent) == expected


_VERIFIED = {}


def path(role, root=ROOT, roles=None):
    """Verified directory of a role's locked component, or SystemExit with what to do."""
    # Real roles are verified once per process; the stat check is slow on /mnt/c.
    key = (role, prepared_root(), os.environ.get(LINE_VARIABLE)) if root == ROOT and roles is None else None
    if key in _VERIFIED:
        return _VERIFIED[key]
    try:
        identity = resolve(role, root=root, roles=roles)
    except ComponentError as error:
        raise SystemExit(f"主机测试组件 / host component {role}: {error}") from None
    target = prepared_root() / identity.dirname
    if not _trusted(identity, target):
        problem = check_prepared(identity)
        if problem:
            raise SystemExit(instructions(identity, problem))
    if key is not None:
        _VERIFIED[key] = target
    return target
