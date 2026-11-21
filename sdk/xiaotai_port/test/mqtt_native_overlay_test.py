# SPDX-License-Identifier: MIT
"""Verify the single native MQTT source plan, optionally in a real IDF compiler context."""

import argparse
from hashlib import sha256
import json
from pathlib import Path
import shlex
import subprocess
import sys
from tempfile import TemporaryDirectory


HERE = Path(__file__).resolve().parent
PORT = HERE.parent
ROOT = PORT.parents[1]
sys.path.insert(0, str(PORT / "esp-idf"))
from mqtt_native_overlay import PINS, SOURCES, write_build_candidate, write_candidate  # noqa: E402


def rejected(call, description):
    try:
        call()
    except ValueError:
        return
    raise AssertionError(f"accepted {description}")


def syntax(candidate, database):
    entries = json.loads(database.read_text(encoding="utf-8"))
    manifest = json.loads((candidate / "manifest.json").read_text(encoding="utf-8"))
    required = list(manifest["sources"])
    optional = ("mqtt5_client.c", "lib/mqtt5_msg.c")
    for name in optional:
        if any(row["file"].endswith("/" + name) for row in entries):
            required.append(name)
    checks = [(name, name, candidate / name) for name in required]
    checks.append(("port adapter", "mqtt_client.c", PORT / "esp-idf/mqtt.c"))
    for label, name, source in checks:
        rows = [row for row in entries if row["file"].endswith("/espressif__mqtt/" + name)]
        if len(rows) != 1:
            raise ValueError(f"expected one native MQTT compile command for {name}")
        row = rows[0]
        original = row.get("arguments") or shlex.split(row["command"])
        command = [original[0]]
        for include in [*(candidate / item for item in manifest["include_dirs"]),
                        Path(manifest["port_include_dir"])]:
            command.extend(("-I", str(include)))
        if label == "port adapter":
            command.append("-DCONFIG_XIAOTAI_PORT_MQTT_NATIVE_OVERLAY=1")
            for include in (PORT / "include", PORT / "esp-idf/include", PORT / "common"):
                command.extend(("-I", str(include)))
        skip = False
        for argument in original[1:]:
            if skip:
                skip = False
            elif argument in ("-o", "-MF", "-MT", "-MQ"):
                skip = True
            elif argument not in ("-c", "-MD", "-MMD", row["file"]):
                command.append(argument)
        command.extend(("-fsyntax-only", str(source)))
        subprocess.run(command, cwd=row["directory"], check=True, timeout=30)
    print(f"PASS: {len(required)} actual native translation units and port adapter share compiler context (syntax only)")


def verify(component, database):
    originals = {name: (component / name).read_bytes() for name in PINS}
    with TemporaryDirectory(prefix="xiaotai-mqtt-native-plan-") as directory:
        scratch = Path(directory)
        output = scratch / "generated"
        manifest = write_candidate(component, output)
        assert manifest["enables_product"] is False
        assert manifest["verification"] == "source_generation_only"
        assert manifest["sources"] == list(SOURCES)
        assert set(manifest["original_sha256"]) == set(originals)
        for name, digest in manifest["generated_sha256"].items():
            assert sha256((output / name).read_bytes()).hexdigest() == digest
        before = (output / "manifest.json").read_bytes()
        rejected(lambda: write_candidate(component, output), "overwriting source evidence")
        assert (output / "manifest.json").read_bytes() == before
        second = write_candidate(component, scratch / "second")
        assert manifest == second
        build_root = scratch / "build"
        reusable = write_build_candidate(component, build_root)
        stamp = (reusable / "manifest.json").stat().st_mtime_ns
        assert write_build_candidate(component, build_root) == reusable
        assert (reusable / "manifest.json").stat().st_mtime_ns == stamp
        unexpected = reusable / "include/esp_event.h"
        unexpected.write_bytes(b"unexpected header\n")
        rejected(lambda: write_build_candidate(component, build_root), "an unlisted build header")
        unexpected.unlink()
        altered_header = reusable / "lib/include/mqtt_msg.h"
        altered_header.write_bytes(altered_header.read_bytes() + b"x")
        rejected(lambda: write_build_candidate(component, build_root), "a changed generated header")
        rejected(lambda: write_candidate(component, component / "unsafe-generated"), "native source output")
        rejected(lambda: write_candidate(component, PORT / "esp-idf/unsafe-generated"), "port source output")
        # The entire original input plan is authenticated before any file is emitted.
        altered = scratch / "altered"
        for name, data in originals.items():
            path = altered / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        for ordinal, (name, data) in enumerate(originals.items()):
            (altered / name).write_bytes(data + b"x")
            bad_output = scratch / f"bad-{ordinal}"
            rejected(lambda: write_candidate(altered, bad_output), f"altered native input {name}")
            assert not bad_output.exists()
            (altered / name).write_bytes(data)
        if database:
            syntax(output, database)
    for name, data in originals.items():
        assert (component / name).read_bytes() == data
    print(f"PASS: native source plan, {len(PINS)} input pins, deterministic output and no vendor writes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mqtt-source", type=Path,
                        default=ROOT / "build/host-components/espressif__mqtt_1.1.0_fb18bc3b")
    parser.add_argument("--compile-db", type=Path)
    args = parser.parse_args()
    try:
        verify(args.mqtt_source.resolve(strict=True), args.compile_db.resolve(strict=True) if args.compile_db else None)
    except subprocess.CalledProcessError as error:
        parser.exit(1, f"FAIL: native MQTT compiler check exited {error.returncode}\n")
    except subprocess.TimeoutExpired:
        parser.exit(1, "FAIL: native MQTT compiler check timed out\n")
    except (OSError, ValueError) as error:
        parser.exit(1, f"FAIL: native MQTT source plan: {error}\n")


if __name__ == "__main__":
    main()
