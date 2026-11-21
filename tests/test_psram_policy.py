"""Memory placement is a capability policy: every product with PSRAM gets the same one.

config/capabilities/psram.defaults is applied by the build entry to each product
whose board layers turn on PSRAM. This checks, for every product and ESP-IDF line:
- products with PSRAM end up with the whole policy, products without get none of it;
- no board file restates a policy setting (one place to change it);
- esp-mqtt still routes its three payload allocations through the options the
  policy sets, so the options really move those buffers.
"""
# Replaces legacy: tests/test_psram_policy.py
from pathlib import Path
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.append(str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "scripts"))
import host_components
import sdk_selection
import xiaotai_build as build
import xiaotai_build_native as native

POLICY = build.read_config(ROOT / "config/capabilities/psram.defaults")
LEGACY_AFE = "CONFIG_XIAOTAI_AFE_MEMORY_PREFER_PSRAM"
NATIVE_AFE = "CONFIG_XIAOTAI_MEDIA_AFE_MEMORY_EXTERNAL"
ALLOCATIONS = {
    "mqtt_client.c": "heap_caps_malloc(buffer_size, MQTT_BUFFER_MEMORY)",
    "lib/mqtt_msg.c": "heap_caps_calloc(buffer_size, sizeof(uint8_t), MQTT_BUFFER_MEMORY)",
    "lib/mqtt_outbox.c": "heap_caps_malloc(message->len + message->remaining_len, MQTT_OUTBOX_MEMORY)",
}


# What each product's legacy firmware configured on every line it built
# (0f93f4147b2978211f47ce5e80c5739449451892): external RAM, and AFE memory preferring it.
WITH_PSRAM = {"CONFIG_SPIRAM": "y", LEGACY_AFE: "y"}
LEGACY_MEMORY = {"xiaotai-xmini-c3-v3": {"CONFIG_SPIRAM": "n", LEGACY_AFE: "n"}}


def recorded_policy(name, line, config, policy):
    external = config.get("CONFIG_SPIRAM") == "y"
    if external:
        wrong = {k: (config.get(k), v) for k, v in policy.items() if config.get(k) != v}
        assert not wrong, (name, line, wrong)
        # A custom outbox would bypass the selected payload memory policy.
        assert config.get("CONFIG_MQTT_CUSTOM_OUTBOX", "n") == "n", (name, line)
    else:
        leaked = sorted(k for k, v in policy.items() if v == "y" and config.get(k) == "y")
        assert not leaked, (name, line, leaked)
    return external


def native_policy(variant, line, legacy):
    name = variant["name"]
    product = sdk_selection.load_product(ROOT / "products" / name / "product.json")
    selected = sdk_selection.plan(ROOT, product)
    profile = {key: selected[key] for key in ("board", "target")}
    if native.profile_path(ROOT, name).is_file():
        pinned = native.load_profile(name, ROOT)
        if line == pinned["idf_line"]:
            profile = pinned
    board = build.read_config(ROOT / "boards" / profile["board"] / "sdkconfig.board")
    restated = sorted(key for key, value in POLICY.items() if board.get(key) == value)
    assert not restated, (name, "native board restates the shared policy", restated)
    package = build.sdk_package(variant, line, ROOT)
    contract = json.loads(package["contract"].read_text(encoding="utf-8-sig"))
    config = native.defaults(ROOT, profile, selected, package, contract)
    policy = {key: value for key, value in POLICY.items() if key != LEGACY_AFE}
    assert recorded_policy(name, line, config, policy) == (legacy.get("CONFIG_SPIRAM") == "y")
    # V3's media option inherits SPIRAM through Kconfig instead of carrying the legacy symbol.
    assert LEGACY_AFE not in config, (name, line)
    assert config.get(NATIVE_AFE, legacy.get(LEGACY_AFE, "n")) == legacy.get(LEGACY_AFE, "n"), (name, line)


def main():
    with_psram = set()
    lines_checked = 0
    afe_kconfig = (ROOT / "sdk/xiaotai_media/Kconfig").read_text(encoding="utf-8")
    afe = re.search(r"(?m)^\s*config " + NATIVE_AFE.removeprefix("CONFIG_") + r"\s*\n"
                    r"([^\n]*(?:\n(?!\s*(?:config|endmenu)\b)[^\n]*)*)", afe_kconfig)
    assert afe and re.search(r"(?m)^\s*depends on SPIRAM\s*$", afe[1])
    assert re.search(r"(?m)^\s*default y\s*$", afe[1])
    for variant in build.variants():
        legacy = LEGACY_MEMORY.get(variant["name"], WITH_PSRAM)
        for line in build.sdk_lines(ROOT)["lines"]:
            if not native.profile_path(ROOT, variant["name"], line=line).is_file():
                continue
            native_policy(variant, line, legacy)
            if legacy["CONFIG_SPIRAM"] == "y":
                with_psram.add(variant["name"])
            lines_checked += 1
    assert with_psram, "no product with PSRAM found"

    mqtt = host_components.path("mqtt")
    for name, allocation in ALLOCATIONS.items():
        assert allocation in (mqtt / name).read_text(encoding="utf-8"), f"{name}: payload allocation changed"
    config = (mqtt / "lib/include/mqtt_config.h").read_text(encoding="utf-8").replace("\r\n", "\n")
    for option, macro in (("CONFIG_MQTT_BUFFERS_ON_EXTERNAL_MEMORY", "MQTT_BUFFER_MEMORY"),
                          ("CONFIG_MQTT_OUTBOX_DATA_ON_EXTERNAL_MEMORY", "MQTT_OUTBOX_MEMORY")):
        assert f"#ifdef {option}\n#define {macro} MALLOC_CAP_SPIRAM" in config, option
    print(f"PSRAM policy: {lines_checked} legacy and V3 config-layer pairs; "
          f"{len(with_psram)} PSRAM products share one policy; none restated by a board")


if __name__ == "__main__":
    main()
