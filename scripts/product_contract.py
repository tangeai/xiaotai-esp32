"""Build-time hardware/media/interaction contracts, not runtime capability probes."""
import hashlib
import json
from pathlib import Path

CAPABILITIES = {
    "audio.pcm.capture", "audio.pcm.playback", "audio.opus16k20.capture",
    "display.rgb565", "display.mono1", "input.pointer", "input.key",
    "input.encoder", "input.remote", "feedback.rgb", "feedback.audio",
    "camera.capture", "network.ip", "memory.psram", "storage.nvs",
}
FEATURES = {"ai", "device_call", "wechat_voip", "room", "h5_stream", "h5_video", "ai_vision"}


def fields(value, required, context):
    if not isinstance(value, dict) or set(value) != set(required):
        raise ValueError(f"{context}: expected fields {sorted(required)}")


def names(value, allowed, context):
    if (not isinstance(value, list) or any(not isinstance(item, str) for item in value)
            or len(value) != len(set(value)) or set(value) - allowed):
        raise ValueError(f"{context}: unknown, duplicate or invalid values")
    return set(value)


def read(path):
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict) or type(value.get("schema")) is not int or value["schema"] != 1:
        raise ValueError(f"{path}: unsupported contract schema")
    return value


def contained(root, relative):
    root = root.resolve()
    path = (root / relative).resolve()
    if not path.is_relative_to(root) or not path.is_file():
        raise ValueError(f"Contract file missing or outside repository: {relative}")
    return path


def check_hardware(hardware):
    fields(hardware, {"schema", "target", "capabilities", "display"}, "hardware")
    if type(hardware["schema"]) is not int or hardware["schema"] != 1:
        raise ValueError("hardware: unsupported contract schema")
    caps = names(hardware["capabilities"], CAPABILITIES, "hardware capabilities")
    display = hardware["display"]
    fields(display, {"kind", "width", "height"}, "display")
    kind = display["kind"]
    if kind not in {"none", "rgb565", "mono1"}:
        raise ValueError("display: unknown pixel format")
    width, height = display["width"], display["height"]
    if type(width) is not int or type(height) is not int:
        raise ValueError("display: dimensions must be integers")
    displays = caps & {"display.rgb565", "display.mono1"}
    if kind == "none":
        if width != 0 or height != 0 or displays or "input.pointer" in caps:
            raise ValueError("display: headless hardware cannot declare a framebuffer or pointer")
    elif width <= 0 or height <= 0 or displays != {f"display.{kind}"}:
        raise ValueError("display: dimensions and capability disagree")
    if "feedback.audio" in caps and "audio.pcm.playback" not in caps:
        raise ValueError("feedback.audio: no supported playback port")
    return caps


def check_adapter(hardware, adapter, context):
    fields(adapter, {"requires", "one_of", "display"}, context)
    caps = check_hardware(hardware)
    required = names(adapter["requires"], CAPABILITIES, f"{context} requirements")
    missing = required - caps
    if missing:
        raise ValueError(f"{context}: missing capabilities {sorted(missing)}")
    if not isinstance(adapter["one_of"], list):
        raise ValueError(f"{context}: one_of must be a list of alternatives")
    for group in adapter["one_of"]:
        choices = names(group, CAPABILITIES, f"{context} alternatives")
        if not choices or not caps & choices:
            raise ValueError(f"{context}: needs one of {sorted(choices)}")
    if adapter["display"] is not None:
        fields(adapter["display"], {"kind", "width", "height"}, f"{context} display")
        if adapter["display"] != hardware["display"]:
            raise ValueError(f"{context}: display geometry/format mismatch")


def check_combination(hardware, media, interaction, features):
    """No board-name/chip-name inference and no implicit codec or UI fallback."""
    caps = check_hardware(hardware)
    enabled = names(features, FEATURES, "product features")
    required = {"network.ip", "storage.nvs"}
    if "h5_video" in enabled:
        if "h5_stream" not in enabled:
            raise ValueError("h5_video requires h5_stream")
        required.add("camera.capture")
    if "ai_vision" in enabled:
        if "ai" not in enabled:
            raise ValueError("ai_vision requires ai")
        required.add("camera.capture")
    if required - caps:
        raise ValueError(f"product: missing capabilities {sorted(required - caps)}")
    check_adapter(hardware, media, "media")
    check_adapter(hardware, interaction, "interaction")


def contract_paths(variant, root):
    return [contained(root, name) for name in (
        f"main/boards/{variant['board']}/capabilities.json",
        "config/adapter-contracts.json", "config/product-contracts.json",
        "scripts/product_contract.py")]


def validate_variant(variant, root):
    paths = contract_paths(variant, root)
    hardware, adapters, products = (read(path) for path in paths[:3])
    fields(adapters, {"schema", "media", "interaction"}, "adapters")
    fields(products, {"schema", "products"}, "products")
    if variant["name"] not in products["products"]:
        raise ValueError(f"No product contract: {variant['name']}")
    product = products["products"][variant["name"]]
    fields(product, {"board", "profile", "ui", "audio", "features", "media", "interaction",
                     "sdk_variant", "portability"}, "product")
    # Existing profiles still own board-specific code. A matching capability set
    # is necessary, but cannot authorize reusing a source graph with old pin maps.
    if product["portability"] != "native-bound" or product["board"] != variant["board"]:
        raise ValueError("Native source profile is board-bound; hardware port migration is required")
    profile = variant["xiaotai"]
    for field in ("ui", "audio", "profile"):
        if product[field] != profile.get(field):
            raise ValueError(f"Product {field} disagrees with preserved source profile")
    if names(product["features"], FEATURES, "preserved features") != names(profile["features"], FEATURES, "selected features"):
        raise ValueError("Product feature baseline changed; no automatic feature trimming")
    if hardware["target"] != variant["target"]:
        raise ValueError("Hardware target disagrees with selected SDK target")
    expected_sdk = "xmini-mini" if profile.get("profile") == "xmini" else "generic"
    if variant["board"] == "lckfb/szpi-esp32s3":
        expected_sdk = "szpi-http"
    if product["sdk_variant"] != expected_sdk:
        raise ValueError("SDK variant disagrees with source/API contract")
    for kind in ("media", "interaction"):
        if product[kind] not in adapters[kind]:
            raise ValueError(f"Unknown {kind} adapter: {product[kind]}")
    check_combination(hardware, adapters["media"][product["media"]],
                      adapters["interaction"][product["interaction"]], product["features"])
    return {"schema": 1, "hardware_contract_checked": True,
            "runtime_portable": False, "hardware_tested": False,
            "media": product["media"], "interaction": product["interaction"],
            "sdk_variant": product["sdk_variant"],
            "inputs": {path.relative_to(root.resolve()).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                       for path in paths}}
