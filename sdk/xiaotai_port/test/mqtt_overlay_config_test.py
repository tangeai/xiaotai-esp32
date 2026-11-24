# SPDX-License-Identifier: MIT
"""Configure the real port source graph without compiling or enabling a product."""

import argparse
from hashlib import sha256
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent / "esp-idf"))
from mqtt_native_overlay import PINS  # noqa: E402


def configure(component, directory, mode, expected_error=None):
    result = subprocess.run([
        "cmake", "-S", str(HERE / "mqtt_overlay_project"), "-B", str(directory),
        f"-DMQTT_DIR={component}", f"-Dmode={mode}", f"-DPython3_EXECUTABLE={sys.executable}",
    ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
    if expected_error:
        if not result.returncode or expected_error not in " ".join(result.stdout.split()):
            raise AssertionError(f"{mode}: expected {expected_error!r}\n{result.stdout}")
    elif result.returncode:
        raise AssertionError(f"{mode}: configuration failed\n{result.stdout}")


def verify(component):
    hashes = {name: sha256((component / name).read_bytes()).hexdigest() for name in PINS}
    with TemporaryDirectory(prefix="xiaotai-mqtt-cmake-") as directory:
        scratch = Path(directory)
        for mode in ("new", "patched", "legacy", "early", "mqtt5_sources"):
            configure(component, scratch / mode, mode)
        manifests = list((scratch / "patched/xiaotai-native-mqtt").glob("*/manifest.json"))
        assert len(manifests) == 1
        before = manifests[0].read_bytes(), manifests[0].stat().st_mtime_ns
        configure(component, scratch / "patched", "patched")
        assert (manifests[0].read_bytes(), manifests[0].stat().st_mtime_ns) == before
        configure(component, scratch / "missing", "missing_mqtt", "V3 MQTT requires")
        configure(component, scratch / "extra", "extra_source", "Unreviewed MQTT source registration")
        altered = scratch / "altered-component"
        for name in PINS:
            path = altered / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((component / name).read_bytes())
        client = altered / "mqtt_client.c"
        client.write_bytes(client.read_bytes() + b"\nx\n")
        configure(altered, scratch / "bad-pin", "patched", "SHA-256 differs")
        configure(altered, scratch / "stock-no-overlay-pins", "new")
    for name, digest in hashes.items():
        assert sha256((component / name).read_bytes()).hexdigest() == digest
    print("PASS: 9 native MQTT source-graph scenarios, exact reconfigure reuse, no vendor writes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mqtt-source", type=Path,
                        default=ROOT / "build/host-components/espressif__mqtt_1.1.0_fb18bc3b")
    args = parser.parse_args()
    try:
        verify(args.mqtt_source.resolve(strict=True))
    except (OSError, ValueError, AssertionError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"FAIL: native MQTT source graph: {error}\n")


if __name__ == "__main__":
    main()
