"""Pin each validated ESP-IDF release by its official Git commit or source-tree digest."""
import hashlib
import os
from pathlib import Path
import subprocess

LOCKS = {
    "5.5.5": {"git_commit": "b774170ff46c393eeb5e495ea37936038d3f4f4f"},
    "6.0.2": {"git_commit": "7101770dc6db2667b3c477cc31365dd1acd6db4e"},
    "6.0.3": {"git_commit": "76f5dedd9950a3012fee8fb7d5586df21fc67802"},
}



def active_version(root):
    """Return MAJOR.MINOR.PATCH of the IDF tree at root, or None when it is not an IDF tree."""
    path = Path(root) / "tools/cmake/version.cmake"
    if not path.is_file():
        return None
    parts = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        for name in ("MAJOR", "MINOR", "PATCH"):
            prefix = f"set(IDF_VERSION_{name} "
            if line.startswith(prefix):
                parts[name] = line[len(prefix):].rstrip(")").strip()
    if set(parts) != {"MAJOR", "MINOR", "PATCH"}:
        return None
    return f"{parts['MAJOR']}.{parts['MINOR']}.{parts['PATCH']}"


def source_identity(root):
    root = Path(root)
    paths = [root / name for name in ("CMakeLists.txt", "Kconfig", "sdkconfig.rename")]
    for name in ("components", "tools"):
        directory = root / name
        if not directory.is_dir():
            raise ValueError(f"Missing IDF source directory: {directory}")
        for parent, directories, files in os.walk(directory):
            directories[:] = [item for item in directories if item not in {".git", "__pycache__"}]
            paths.extend(Path(parent) / item for item in files if not item.endswith((".pyc", ".pyo")))
    digest = hashlib.sha256()
    for path in sorted(paths, key=lambda item: item.relative_to(root).as_posix()):
        with path.open("rb") as stream:
            content = hashlib.file_digest(stream, "sha256").hexdigest()
        digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        digest.update(content.encode("ascii") + b"\n")
    return {"source_tree_sha256": digest.hexdigest(), "source_file_count": len(paths)}


def verify(root, version):
    if version not in LOCKS:
        raise ValueError(f"No validated IDF environment for {version}")
    root = Path(root)
    if not (root / "tools/idf.py").is_file():
        raise ValueError(f"Activate the pinned ESP-IDF {version} environment first")
    lock = LOCKS[version]
    if "git_commit" in lock:
        try:
            actual = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True,
                                    text=True, check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            raise ValueError(f"ESP-IDF {version} here differs from the validated checkout: {root} is not a Git "
                             f"checkout of the official tag; install the official v{version}") from None
        if actual != lock["git_commit"]:
            raise ValueError(f"ESP-IDF {version} here differs from the validated commit {lock['git_commit']}: {actual}")
        return {"version": version, "git_commit": actual}
    try:
        identity = source_identity(root)
    except OSError as error:
        raise ValueError(f"Cannot read the ESP-IDF {version} tree: {error}") from error
    if identity["source_tree_sha256"] != lock["source_tree_sha256"]:
        # A digest names one exact tree; an official install or another OS's checkout differs.
        raise ValueError(f"ESP-IDF {version} here differs from the validated tree in build image {lock['builder_image']}; "
                         f"build this product in that image, or use another ESP-IDF line (see --list-idf): {identity}")
    return {"version": version, **identity}
