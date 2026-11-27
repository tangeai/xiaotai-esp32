# SPDX-License-Identifier: MIT
"""Verify the native one-slot dispatch prerequisite; no port activation or IO."""
import argparse
from hashlib import sha256
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PINNED = "83e5a7065b8774dce83dd6b9be0163caed3a4fdbbd7de38b328e7e01911487dc"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mqtt-source", type=Path,
                        default=ROOT / "build/host-components/espressif__mqtt_1.1.0_fb18bc3b")
    args = parser.parse_args()
    source = (args.mqtt_source / "mqtt_client.c").read_bytes()
    if sha256(source).hexdigest() != PINNED:
        parser.error("unreviewed native MQTT client SHA-256")
    marker = b"static esp_err_t esp_mqtt_dispatch_event(esp_mqtt_client_handle_t client)\n{"
    assert source.count(marker) == 1
    start = source.index(marker)
    end = source.index(b"\n}\n", start) + 3
    with tempfile.TemporaryDirectory(prefix="xiaotai-mqtt-mailbox-") as name:
        scratch = Path(name)
        (scratch / "native_dispatch.inc").write_bytes(source[start:end])
        binary = scratch / "mailbox"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-pthread", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-pie", "-no-pie",
                        f"-I{scratch}", f"-I{HERE.parent / 'esp-idf'}",
                        str(HERE / "mqtt_owner_mailbox_test.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})
    print("PASS: native full-queue wait reproduced; out-of-band mailbox coalesces and isolates epochs")


if __name__ == "__main__":
    main()
