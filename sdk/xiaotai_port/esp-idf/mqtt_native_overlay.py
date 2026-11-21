# SPDX-License-Identifier: MIT
"""Generate one consistent native MQTT candidate from exact supported inputs.

This is source generation, not an SDK build or product enablement. The original
component is read-only. Changed and unchanged native translation units use one
generated header tree; tests must not mix a stock codec with this client.
"""

import argparse
from hashlib import sha256
import json
from pathlib import Path

from mqtt_cleanup_overlay import patch_client_cleanup, patch_msg_cleanup
from mqtt_overlay_common import CLIENT_SHA256, PRIVATE_HEADER_SHA256, locked_bytes
from mqtt_id_overlay import (PINS as ID_PINS, overlay_msg_header, overlay_msg_source,
                             overlay_outbox_header, overlay_outbox_source)
from mqtt_owner_overlay import owner_header, owner_source
from mqtt_queue_overlay import (patch_client_queue, patch_outbox_queue,
                                patch_outbox_queue_header, queue_publish_transaction)


HERE = Path(__file__).resolve().parent
PINS = {
    "LICENSE": "20ed622a5f1a273258b415097609f6a236fb2f23f8c593c63e29e6cc77de0032",
    "mqtt_client.c": CLIENT_SHA256,
    "include/mqtt_client.h": "da851009a71d3e381f7dd4973b84a0f0499f7090a8aaf215572ad7aaba60e179",
    "include/mqtt_supported_features.h": "4f9a6d7ac6e01517722861a16276df8dfef9791339e35c169a57691b909a85f8",
    "lib/include/mqtt_client_priv.h": PRIVATE_HEADER_SHA256,
    "lib/include/mqtt_config.h": "b16062e703223bfff86e9834825411352e24fd4f9c185bd6e1b75c7905253cc7",
    "lib/include/platform.h": "95fe9b4feb2c78977a3506b6772997b64e58a173897d370d61ba0468a91b8d45",
    "lib/include/mqtt_msg.h": ID_PINS["msg_header"],
    "lib/mqtt_msg.c": ID_PINS["msg_source"],
    "lib/include/mqtt_outbox.h": ID_PINS["outbox_header"],
    "lib/mqtt_outbox.c": ID_PINS["outbox_source"],
    "lib/platform_esp32_idf.c": "cd94d14de26dddc0498683cb512e41e86bfa9d227e899de7d84aeadce95d4d28",
    "lib/include/platform_esp32_idf.h": "c7b3dde6f103bb0af86a653b0e263d57011b85d44e1264e69df7731a7631e80a",
    "lib/mqtt_utils/mqtt_utils.c": "b59a589d9ce4aab80445eabc40ee070439834c26ae0f51151477b1af1794d3d7",
    "lib/mqtt_utils/include/mqtt_utils.h": "9b49beb2592e31c206c337a33802c195d1957305da20916d5c4999a9492b3cba",
    "mqtt5_client.c": "8fecc7182072eaa360a0ae1acbaa9e650cf312f78fea3d399af44080af752f3b",
    "include/mqtt5_client.h": "93fce4cdcc0ed372f80866102696d4fe47f2fdd5e7649057648e6d549ffe43b4",
    "lib/mqtt5_msg.c": "c7166e1bce335b9803dfc27c23899bc3ceec921b5c60aba7b59160d2af6eb2a0",
    "lib/include/mqtt5_client_priv.h": "ab530bef1f1e35034fbe3ad907869527abb29a4ccd64000608da5a8cd4e634c5",
    "lib/include/mqtt5_msg.h": "55a003857dc532f5e71b45b0aa8329c9827ff7db9b35f32e6c3406a943ad79a7",
}
SOURCES = ("mqtt_client.c", "lib/mqtt_msg.c", "lib/platform_esp32_idf.c",
           "lib/mqtt_utils/mqtt_utils.c", "lib/mqtt_outbox.c")


def plan(component):
    """Validate every original input before producing any generated files."""
    raw = {name: locked_bytes(component / name, digest) for name, digest in PINS.items()}
    generated = dict(raw)
    client = patch_client_cleanup(patch_client_queue(raw["mqtt_client.c"], owner_source(raw["mqtt_client.c"])))
    transaction = queue_publish_transaction(client)
    prefix = (b'#include "sdkconfig.h"\n'
              b'#if CONFIG_MQTT_CUSTOM_OUTBOX\n'
              b'#error "XiaoTai MQTT requires the supported native outbox"\n'
              b'#endif\n#define XIAOTAI_MQTT_NATIVE_ID_RESERVATION 1\n')
    generated["mqtt_client.c"] = (
        prefix + client + b'\n#line 1 "mqtt_enqueue_connected.inc"\n' +
        (HERE / "mqtt_enqueue_connected.inc").read_bytes() +
        b'\n#line 1 "mqtt_publish_transaction"\n' + transaction +
        b'\n#line 1 "mqtt_publish_now.inc"\n' + (HERE / "mqtt_publish_now.inc").read_bytes())
    generated["lib/include/mqtt_client_priv.h"] = owner_header(raw["lib/include/mqtt_client_priv.h"])
    generated["lib/include/mqtt_msg.h"] = overlay_msg_header(raw["lib/include/mqtt_msg.h"])
    generated["lib/include/mqtt_outbox.h"] = patch_outbox_queue_header(
        raw["lib/include/mqtt_outbox.h"], overlay_outbox_header(raw["lib/include/mqtt_outbox.h"]))
    generated["lib/mqtt_msg.c"] = patch_msg_cleanup(overlay_msg_source(raw["lib/mqtt_msg.c"]))
    generated["lib/mqtt_outbox.c"] = patch_outbox_queue(
        raw["lib/mqtt_outbox.c"], overlay_outbox_source(raw["lib/mqtt_outbox.c"]))
    return generated


def metadata(component, generated):
    helpers = sorted(path for path in HERE.glob("mqtt*")
                     if path.is_file() and path.suffix in {".py", ".inc", ".h", ".c", ".cmake"})
    return {
        "kind": "xiaotai-native-mqtt-source-candidate",
        "enables_product": False,
        "verification": "source_generation_only",
        "port_protocol": "3.1.1",
        "source_component": str(component),
        "original_sha256": PINS,
        "generated_sha256": {name: sha256(data).hexdigest() for name, data in generated.items()},
        "port_sha256": {path.name: sha256(path.read_bytes()).hexdigest() for path in helpers},
        "sources": list(SOURCES),
        "optional_sources": {"CONFIG_MQTT_PROTOCOL_5": ["mqtt5_client.c", "lib/mqtt5_msg.c"]},
        "include_dirs": ["include", "lib/include", "lib/mqtt_utils/include"],
        "port_include_dir": str(HERE),
    }


def write_output(component, output, generated, manifest, reuse):
    output = output.resolve()
    if (output == component or component in output.parents or output in component.parents or
            output == HERE or HERE in output.parents):
        raise ValueError("generate the candidate outside the original component and port sources")
    # A build may reuse exact verified bytes. It never repairs or overwrites a
    # damaged result; changed inputs instead select a new content-addressed path.
    if output.exists() and any(output.iterdir()):
        if not reuse:
            raise ValueError("candidate output directory must be empty")
        previous = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        if previous != manifest:
            raise ValueError("existing native source manifest differs")
        paths = list(output.rglob("*"))
        actual = {path.relative_to(output).as_posix() for path in paths if path.is_file()}
        if any(path.is_symlink() for path in paths) or actual != set(generated) | {"manifest.json"}:
            raise ValueError("unexpected files in the generated native source tree")
        for name, digest in manifest["generated_sha256"].items():
            if sha256((output / name).read_bytes()).hexdigest() != digest:
                raise ValueError(f"existing generated native source changed: {name}")
        return
    output.mkdir(parents=True, exist_ok=True)
    for name, data in generated.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as stream:
            stream.write(data)
    with (output / "manifest.json").open("x", encoding="utf-8") as stream:
        json.dump(manifest, stream, indent=2)
        stream.write("\n")


def write_candidate(component, output):
    component = component.resolve(strict=True)
    generated = plan(component)
    manifest = metadata(component, generated)
    write_output(component, output, generated, manifest, False)
    return manifest


def write_build_candidate(component, build_root):
    component = component.resolve(strict=True)
    generated = plan(component)
    manifest = metadata(component, generated)
    identity = sha256(json.dumps(manifest, sort_keys=True).encode("utf-8")).hexdigest()
    output = build_root.resolve() / identity
    write_output(component, output, generated, manifest, True)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--build-root", type=Path, help="reuse exact content-addressed build sources")
    args = parser.parse_args()
    if (args.output is None) == (args.build_root is None):
        parser.error("provide output or --build-root, not both")
    try:
        if args.build_root:
            output = write_build_candidate(args.component, args.build_root)
        else:
            manifest = write_candidate(args.component, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"FAIL: native MQTT source generation: {error}\n")
    if args.build_root:
        print(output)
    else:
        print(f"Generated {len(manifest['generated_sha256'])} native source files; no product settings changed")


if __name__ == "__main__":
    main()
