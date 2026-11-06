"""Pin both Git checkouts and the source-only IDF used by the S3 producer."""
import hashlib
import os
from pathlib import Path
import subprocess

LOCKS = {
    "6.0.2": {"git_commit": "7101770dc6db2667b3c477cc31365dd1acd6db4e"},
    "6.0.3": {
        "source_tree_sha256": "e417314424b4ca5b7ebcd666a799e881d569ac29b85d19ae64aaa3c47e459d5c",
        "builder_image": "sha256:c3946169367028848ff6981bdfab219572ad75f81b856eb5006c9a9060dc9dd8",
    },
}


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
        actual = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
        if actual != lock["git_commit"]:
            raise ValueError(f"IDF checkout is not the validated {version} commit: {actual}")
        return {"version": version, "git_commit": actual}
    identity = source_identity(root)
    if identity["source_tree_sha256"] != lock["source_tree_sha256"]:
        raise ValueError(f"IDF {version} source tree differs from the validated producer: {identity}")
    return {"version": version, **identity}
