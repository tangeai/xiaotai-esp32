# SPDX-License-Identifier: MIT
"""Rehearse native MQTT packet-ID reservation without activating any production code."""

import argparse
from hashlib import sha256
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
sys.path.insert(0, str(HERE.parents[1] / "esp-idf"))
from mqtt_id_overlay import (overlay_msg_header, overlay_msg_source, overlay_outbox_header,
                             overlay_outbox_source)  # noqa: E402
sys.path.insert(0, str(HERE.parent))
from mqtt_native_fixture import excerpt, write_compat

PINS = {
    "mqtt_client.c": "83e5a7065b8774dce83dd6b9be0163caed3a4fdbbd7de38b328e7e01911487dc",
    "include/mqtt_client.h": "da851009a71d3e381f7dd4973b84a0f0499f7090a8aaf215572ad7aaba60e179",
    "include/mqtt_supported_features.h": "4f9a6d7ac6e01517722861a16276df8dfef9791339e35c169a57691b909a85f8",
    "lib/include/mqtt_client_priv.h": "dc613a10765ad9d68648065400fb2c52bb081cca1951c64f7e6b7df574971c49",
    "lib/include/mqtt_config.h": "b16062e703223bfff86e9834825411352e24fd4f9c185bd6e1b75c7905253cc7",
    "lib/include/platform.h": "95fe9b4feb2c78977a3506b6772997b64e58a173897d370d61ba0468a91b8d45",
}
OVERLAYS = {
    "lib/include/mqtt_msg.h": overlay_msg_header,
    "lib/mqtt_msg.c": overlay_msg_source,
    "lib/include/mqtt_outbox.h": overlay_outbox_header,
    "lib/mqtt_outbox.c": overlay_outbox_source,
}
FUNCTIONS = (
    "static inline void mqtt_reset_pending_message(",
    "static bool remove_initiator_message(",
    "static outbox_item_handle_t mqtt_enqueue(",
    "static esp_err_t mqtt_resend_queued(",
    "int esp_mqtt_client_subscribe_multiple(",
    "int esp_mqtt_client_unsubscribe(",
    "static int make_publish(",
    "static inline int mqtt_client_enqueue_publish(",
    "int esp_mqtt_client_enqueue(",
)


def prove(native, compiler):
    raw = {}
    for name, digest in PINS.items():
        raw[name] = (native / name).read_bytes()
        if sha256(raw[name]).hexdigest() != digest:
            raise ValueError(f"native {name} SHA-256 mismatch")
    patched = {}
    for name, patch in OVERLAYS.items():
        raw[name] = (native / name).read_bytes()
        patched[name] = patch(raw[name])
        try:
            patch(raw[name] + b"x")
        except ValueError:
            pass
        else:
            raise AssertionError(f"{name}: accepted an altered native input")
    with tempfile.TemporaryDirectory(prefix="xiaotai-pw7-ids-") as directory:
        root = Path(directory)
        for variant in ("stock", "reserved"):
            source = root / variant
            for name, content in raw.items():
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(patched.get(name, content) if variant == "reserved" else content)
            (source / "native_outbox.c").write_bytes((source / "lib/mqtt_outbox.c").read_bytes())
            client = raw["mqtt_client.c"].decode("utf-8")
            (source / "native_client.inc").write_text("\n".join(excerpt(client, start) for start in FUNCTIONS), encoding="utf-8")
            write_compat(source)
            (source / "random_api.h").write_text("int platform_random(int max);\n", encoding="ascii")
            for incremental in (0, 1):
                (source / "sdkconfig.h").write_text(
                    f"#pragma once\n#define CONFIG_MQTT_PROTOCOL_311 1\n#define CONFIG_MQTT_MSG_ID_INCREMENTAL {incremental}\n",
                    encoding="ascii")
                binary = source / f"proof-{incremental}"
                command = [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                           "-Wno-unused-function", "-D_GNU_SOURCE", f"-DTEST_STOCK={int(variant == 'stock')}",
                           "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-pie", "-no-pie", "-pthread",
                           "-include", "mqtt_native_compat.h", "-include", "random_api.h"]
                for include in (source, source / "include", source / "lib/include", ROOT / "sdk/xiaotai_port/include"):
                    command.extend(("-I", str(include)))
                if variant == "reserved":
                    unsafe = subprocess.run([*command, "-DCONFIG_MQTT_DISABLE_API_LOCKS=1", "-fsyntax-only",
                                             str(source / "lib/mqtt_msg.c")], capture_output=True, text=True, timeout=10)
                    if not unsafe.returncode or "reservation requires the native MQTT API mutex" not in unsafe.stderr:
                        raise AssertionError("reservation did not reject a mutex-free native build")
                subprocess.run([*command, str(HERE / "proof.c"), str(source / "lib/mqtt_msg.c"), "-o", str(binary)],
                               check=True, timeout=30)
                subprocess.run([str(binary)], check=True, timeout=30,
                               env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mqtt-source", type=Path,
                        default=ROOT / "build/host-components/espressif__mqtt_1.1.0_fb18bc3b")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    try:
        prove(args.mqtt_source.resolve(), args.cc)
    except (ValueError, OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"FAIL: PW7 packet-ID proof: {error}\n")


if __name__ == "__main__":
    main()
