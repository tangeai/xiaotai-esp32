"""Read-only artifact audit; run in the same environment as the recorded build."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(folder):
    record = json.loads((folder / "build-record.json").read_text())
    assert record["hardware_tested"] is False
    assert sha256(Path(record["sdkconfig"])) == record["sdkconfig_sha256"], "sdkconfig changed"
    for name, expected in record["artifacts"].items():
        assert sha256(folder / name) == expected, f"Artifact changed: {name}"
    manifest_path = folder / "build-inputs.json"
    assert sha256(manifest_path) == record["input_manifest_sha256"], "Input manifest changed"
    manifest = json.loads(manifest_path.read_text())
    assert len(manifest) == record["input_file_count"]
    for name in ("build.ninja", "compile_commands.json", "CMakeCache.txt", "config/sdkconfig.h", "toolchain/cflags"):
        assert str(folder / name) in manifest, f"Missing effective build input: {name}"
    for name, expected in manifest.items():
        assert sha256(Path(name)) == expected, f"Build input changed: {name}"
    if "dependencies_lock_sha256" in record:
        assert sha256(folder / "dependencies.lock") == record["dependencies_lock_sha256"]
    cache = (folder / "CMakeCache.txt").read_text()
    nm = next(line.split("=", 1)[1] for line in cache.splitlines() if line.startswith("CMAKE_NM:FILEPATH="))
    elf = next(folder.glob("*.elf"))
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
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dirs", nargs="+", type=Path)
    args = parser.parse_args()
    results = [verify(folder.resolve()) for folder in args.build_dirs]
    print(json.dumps({"passed": True, "builds": results}, indent=2))


if __name__ == "__main__":
    main()
