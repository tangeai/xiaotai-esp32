"""Prepare an isolated, pinned C3 source candidate. Never build, flash or publish."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[1]
SUFFIXES = {".c", ".h", ".S", ".py", ".sh", ".ps1", ".a", ".opuspkt", ".json",
            ".html", ".yml", ".yaml", ".cmake"}
TOOLS = {"build.ps1", "test_host.sh", "serial_capture.py", "serial_live.py", "opus_packets.py",
         "prepare_opus.py", "rx_to_wav.py", "regression_summary.py", "generate_interaction_sounds.py",
         "prepare_release.py", "package_release.py", "test_release_packaging.py", "test_wifi_portal.cjs"}
PUBLIC_DOCS = {"README.md", "PORTING.md", "USER_GUIDE.md", "FLASHING.md", "GETTING_STARTED_CN.md", "TESTING.md",
               "SOURCES.md", "RELEASE_NOTES.md",
               "TBOX_LICENSE.txt", "ESP_AUDIO_CODEC_LICENSE.txt", "ESP_IDF_LICENSE.txt"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs(root):
    for folder in ("components", "main", "tests"):
        for path in sorted((root / folder).rglob("*")):
            if not path.is_file() or "__pycache__" in path.parts or ".local." in path.name:
                continue
            if path.suffix in SUFFIXES or path.name in {"CMakeLists.txt", "Kconfig", "Kconfig.projbuild"}:
                if path.is_symlink():
                    raise ValueError(f"Symlink is not a release input: {path}")
                yield path.relative_to(root)
    for name in sorted(TOOLS):
        yield Path("tools") / name
    for name in ("CMakeLists.txt", "partitions.csv", "sdkconfig.defaults", ".clang-format",
                 "dependencies.lock", "README.md", "CHANGELOG.md", "LICENSE.txt", ".gitignore"):
        yield Path(name)
    for name in sorted(PUBLIC_DOCS):
        yield Path("docs") / name
    yield Path("config/release-profile.json")


def prepare(root, output, sdk, sdkconfig):
    profile = json.loads((root / "config/release-profile.json").read_text(encoding="utf-8"))
    if (profile.get("project_name") != "xiaotai_mz28_ai" or
            profile.get("flash_size") != "8MB" or profile.get("status") not in {"pre-release", "release"}):
        raise ValueError("Expected an MZ28 8MB release or pre-release profile")
    if output.exists():
        raise ValueError("Output already exists; choose a new directory (never overwrite evidence)")
    for path, expected in [(sdk, profile["sdk_sha256"]),
                           (sdkconfig, profile["sdkconfig_sha256"])]:
        if digest(path) != expected:
            raise ValueError(f"Unapproved SDK/config pair: {path.name}")
    for name, expected in profile["headers"].items():
        if digest(root / "components/tirtc_sdk/include" / name) != expected:
            raise ValueError(f"SDK header mismatch: {name}")
    app = (root / "main/app_config.h").read_text(encoding="utf-8")
    for field in ("SSID", "PASSWORD"):
        if not re.search(rf'^#define APP_WIFI_{field}\s+""\s*$', app, re.M):
            raise ValueError("Public app_config.h must not contain Wi-Fi credentials")
    if not re.search(r'^#define APP_WECHAT_VERSION\s+WECHAT_VERSION_RELEASE\s*$', app, re.M):
        raise ValueError("Release candidate must select the formal mini-program")
    cmake_text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    if f'project({profile["project_name"]} VERSION {profile["version"]})' not in cmake_text:
        raise ValueError("Project name/version differs from the release profile")
    sdk_lock = json.loads((root / "components/tirtc_sdk/release-lock.json").read_text(encoding="utf-8"))
    if sdk_lock["sdk_sha256"] != profile["sdk_sha256"]:
        raise ValueError("SDK lock differs from the MZ28 release profile")
    selected = list(inputs(root))
    for path in selected:
        if not (root / path).is_file():
            raise ValueError(f"Missing release input: {path}")
    source = output / "source"
    source.mkdir(parents=True)
    for relative in selected:
        destination = source / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / relative, destination)
    # Preserve the exact development inputs, including manifests and the web asset.
    shutil.copy2(sdk, source / "components/tirtc_sdk/lib/libTiRTC.a")
    shutil.copy2(sdkconfig, source / "sdkconfig")
    shutil.copy2(root / "docs/UPSTREAM_LICENSE.txt", source / "docs/UPSTREAM_LICENSE.txt")
    manifest = {"status": profile["status"], "version": profile["version"],
                "source_commit": None, "source_identity": "sha256-file-manifest",
                "project_name": profile["project_name"], "artifact_name": profile["artifact_name"],
                "flash_size": profile["flash_size"], "sdk": profile,
                "files": {p.relative_to(source).as_posix(): digest(p)
                          for p in sorted(source.rglob("*")) if p.is_file()}}
    (output / "source-manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--sdkconfig", required=True, type=Path)
    args = parser.parse_args()
    try:
        source = prepare(ROOT, args.output.resolve(), args.sdk.resolve(), args.sdkconfig.resolve())
    except (OSError, ValueError) as error:
        raise SystemExit(f"Candidate refused: {error}") from error
    print(f"Candidate source: {source}")
    print("Not published. Build this source, record validation and confirm destination before push.")


if __name__ == "__main__":
    main()
