# SPDX-License-Identifier: MIT
"""Prove dormant native credential and CONNECT-buffer cleanup using locked source."""

import argparse
from hashlib import sha256
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
PORT = HERE.parents[1]
ROOT = HERE.parents[3]
sys.path.insert(0, str(PORT / "esp-idf"))
sys.path.insert(0, str(HERE.parent))
from mqtt_cleanup_overlay import overlay_client_cleanup, overlay_msg_cleanup, patch_client_cleanup, patch_msg_cleanup
from mqtt_native_fixture import excerpt, write_compat

PINS = {
    "mqtt_client.c": "83e5a7065b8774dce83dd6b9be0163caed3a4fdbbd7de38b328e7e01911487dc",
    "include/mqtt_client.h": "da851009a71d3e381f7dd4973b84a0f0499f7090a8aaf215572ad7aaba60e179",
    "include/mqtt_supported_features.h": "4f9a6d7ac6e01517722861a16276df8dfef9791339e35c169a57691b909a85f8",
    "lib/include/mqtt_client_priv.h": "dc613a10765ad9d68648065400fb2c52bb081cca1951c64f7e6b7df574971c49",
    "lib/include/mqtt_config.h": "b16062e703223bfff86e9834825411352e24fd4f9c185bd6e1b75c7905253cc7",
    "lib/include/mqtt_outbox.h": "12c5e19841bd0a21e9445d9ffe480103d206255c37272ce43c26d5428783c34f",
    "lib/include/mqtt_msg.h": "c3dbd6b37f168662c995406af2f4119fe2cb869a376667cf11949b9fc3089009",
    "lib/include/platform.h": "95fe9b4feb2c78977a3506b6772997b64e58a173897d370d61ba0468a91b8d45",
    "lib/mqtt_msg.c": "4f030f744316677de0d852968b5c6e8d99dfa9f3e8601c6800385779c05096cc",
    "lib/mqtt_outbox.c": "5e17d0b15f699e241a89b28301d17507003813100e2f61cc4d0906362d51b5e3",
    "lib/mqtt_utils/mqtt_utils.c": "b59a589d9ce4aab80445eabc40ee070439834c26ae0f51151477b1af1794d3d7",
}
FUNCTIONS = (
    "static esp_err_t esp_mqtt_check_cfg_conflict(",
    "bool esp_mqtt_set_if_config(",
    "esp_err_t esp_mqtt_set_config(",
    "void esp_mqtt_destroy_config(",
    "static bool create_client_data(",
    "esp_mqtt_client_handle_t esp_mqtt_client_init(",
    "esp_err_t esp_mqtt_client_destroy(",
)


def prove(native, compiler):
    raw = {}
    for name, digest in PINS.items():
        raw[name] = (native / name).read_bytes()
        if sha256(raw[name]).hexdigest() != digest:
            raise ValueError(f"native {name} SHA-256 mismatch")
    patched_client = overlay_client_cleanup(raw["mqtt_client.c"])
    patched_msg = overlay_msg_cleanup(raw["lib/mqtt_msg.c"])
    for data, patch in ((raw["mqtt_client.c"], overlay_client_cleanup), (raw["lib/mqtt_msg.c"], overlay_msg_cleanup)):
        try:
            patch(data + b"x")
        except ValueError:
            pass
        else:
            raise AssertionError("unlocked source was accepted by native cleanup")
    # Pure composition API does not re-authenticate already transformed input;
    # it still requires every structure anchor to match exactly once.
    prefix = b"/* earlier authenticated transform */\n"
    assert patch_client_cleanup(prefix + raw["mqtt_client.c"]) == prefix + patched_client
    assert patch_msg_cleanup(prefix + raw["lib/mqtt_msg.c"]) == prefix + patched_msg
    for data, patch in ((patched_client, patch_client_cleanup), (patched_msg, patch_msg_cleanup)):
        try:
            patch(data)
        except ValueError:
            pass
        else:
            raise AssertionError("cleanup transform was silently installed twice")

    with tempfile.TemporaryDirectory(prefix="xiaotai-pw7-cleanup-") as directory:
        for stock in (True, False):
            source = Path(directory) / ("stock" if stock else "cleanup")
            for name, content in raw.items():
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
            write_compat(source)
            (source / "native_outbox.c").write_bytes(raw["lib/mqtt_outbox.c"])
            (source / "native_msg.c").write_bytes(raw["lib/mqtt_msg.c"] if stock else patched_msg)
            client = (raw["mqtt_client.c"] if stock else patched_client).decode("utf-8")
            functions = [excerpt(raw["lib/mqtt_utils/mqtt_utils.c"].decode("utf-8"), "char *mqtt_create_string(")]
            if not stock:
                functions.append(excerpt(client, "static void xt_native_mqtt_wipe_string("))
            functions.extend(excerpt(client, start) for start in FUNCTIONS)
            (source / "native_cleanup.inc").write_text("\n".join(functions), encoding="utf-8")
            binary = source / "proof"
            command = [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-D_GNU_SOURCE",
                       f"-DTEST_STOCK={int(stock)}", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-fno-pie", "-no-pie", "-pthread", "-include", "mqtt_native_compat.h"]
            for include in (source, source / "include", source / "lib/include", PORT / "include"):
                command.extend(("-I", str(include)))
            subprocess.run([*command, str(HERE / "proof.c"), "-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=15,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mqtt-source", type=Path,
                        default=ROOT / "build/host-components/espressif__mqtt_1.1.0_fb18bc3b")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    try:
        prove(args.mqtt_source.resolve(), args.cc)
    except (OSError, ValueError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"FAIL: PW7 native cleanup: {error}\n")


if __name__ == "__main__":
    main()
