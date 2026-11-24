"""Product selection: applications -> modules -> engines, needs against board
capabilities, and the generated framework tables running for real on the host."""
# Replaces legacy: tests/test_recognition_components.py
# Replaces legacy: tests/test_native_profiles.py
# Replaces legacy: tests/test_product_contracts.py
# Replaces legacy: tests/test_s3_board_selection.py
from pathlib import Path
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import sdk_selection as selection  # noqa: E402
import product_scope  # noqa: E402
import xiaotai_build_native as native  # noqa: E402


def write(root, relative, data):
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data), encoding="utf-8")


def fails(call, message):
    try:
        call()
    except selection.SelectionError as error:
        assert message in str(error), (message, error)
    else:
        raise AssertionError(f"expected failure: {message}")


with tempfile.TemporaryDirectory(prefix="xiaotai-independent-s3-") as temp:
    root = Path(temp)
    catalog = {path for _, pattern in selection.PATTERNS for path in ROOT.glob(pattern)}
    catalog.update((ROOT / "sdk").rglob("Kconfig"))
    catalog.update((ROOT / "apps").rglob("Kconfig"))
    for source in catalog:
        target = root / source.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    for base in (ROOT / "sdk", ROOT / "sdk/parts"):
        for directory in base.iterdir():
            if directory.is_dir():
                (root / directory.relative_to(ROOT)).mkdir(parents=True, exist_ok=True)
    reference = selection.load_product(ROOT / "products/xiaotai-szpi-s3/product.json")
    board_name = "fixture/s3-voice"
    product_name = "fixture-s3-voice"
    reference.update(board=board_name, name=product_name)
    fails(lambda: selection.plan(root, reference), "missing manifest")
    board = json.loads((ROOT / "boards/lckfb/szpi-esp32s3/board.json").read_text(encoding="utf-8"))
    board["identity"]["model"] = product_name
    del board["parts"]["camera"]
    board["components"].remove("xiaotai_part_camera_dvp")
    write(root, f"boards/{board_name}/board.json", board)
    write(root, f"products/{product_name}/product.json", reference)
    product = selection.load_product(root / "products" / product_name / "product.json")
    fails(lambda: selection.plan(root, product), "camera")
    out = root / "generated"
    plan = selection.write(root, selection.apply_variant(product, "no-video"), out)
    assert (plan["product"], plan["board"], plan["target"]) == (product_name, board_name, "esp32s3")
    assert {"ai_talk", "stream", "call", "wechat", "room"} <= set(plan["modules"])
    assert not {"video", "ai_vision", "stream_video"}.intersection(plan["modules"])
    assert "xiaotai_part_camera_dvp" not in plan["components"]
    assert f'set(XIAOTAI_BOARD "{board_name}")' in (out / "selection.cmake").read_text(encoding="utf-8")
    try:
        native.native_plan(None, {"name": product_name}, {}, root=root)
    except ValueError as error:
        assert "no locked native profile" in str(error), error
    else:
        raise AssertionError("independent development product must not use the locked release entry")


with tempfile.TemporaryDirectory(prefix="xiaotai-board-identity-") as temp:
    root = Path(temp)
    board = {"schema": 3, "identity": {"model": 'fixture"\\', "revision": "V1.2"},
             "parts": {"input": [{"kind": "button"}]}}
    manifest = root / "board.json"
    write(root, "board.json", board)
    header = selection.board_header(manifest)
    assert '#define XIAOTAI_BOARD_MODEL "fixture\\\"\\\\"\n' in header, header
    assert '#define XIAOTAI_BOARD_PROFILE_MODEL nullptr\n' in header, header
    assert f'#define XIAOTAI_BOARD_MANIFEST_NEEDS 0x{selection.NEEDS["key"]:x}u\n' in header, header
    board["identity"]["revision"] = ""
    write(root, "board.json", board)
    fails(lambda: selection.board_header(manifest), "identity.revision")
    board["schema"] = 2
    write(root, "board.json", board)
    fails(lambda: selection.board_header(manifest), "schema 3")


with tempfile.TemporaryDirectory(prefix="xiaotai-selection-") as temp:
    root = Path(temp)
    write(root, "sdk/xiaotai_platform/engine.json", {"name": "platform", "start": True})
    write(root, "sdk/xiaotai_media/engine.json", {"name": "media", "start": True})
    write(root, "sdk/xiaotai_transport/engine.json", {"name": "transport", "depends": ["platform"]})
    write(root, "sdk/xiaotai_wake/engine.json", {"name": "wake_engine", "depends": ["media"]})
    write(root, "sdk/xiaotai_bad_engine/engine.json", {"name": "bad_engine", "depends": ["binding"]})
    write(root, "sdk/modules/xiaotai_mod_provisioning/module.json",
          {"name": "provisioning", "needs": ["network"]})
    write(root, "sdk/modules/xiaotai_mod_binding/module.json",
          {"name": "binding", "depends": ["platform", "provisioning"], "start": True})
    write(root, "sdk/modules/xiaotai_mod_ai_talk/module.json",
          {"name": "ai_talk", "depends": ["media", "transport", "binding"], "needs": ["audio_in", "audio_out"],
           "activities": {"ai": 2}})
    write(root, "sdk/modules/xiaotai_mod_wake/module.json",
          {"name": "wake", "depends": ["wake_engine", "ai_talk"], "needs": ["pcm_in"]})
    write(root, "sdk/modules/xiaotai_mod_lock/module.json", {"name": "lock", "needs": ["door_lock"]})
    write(root, "sdk/modules/xiaotai_mod_loop_a/module.json", {"name": "loop_a", "depends": ["loop_b"]})
    write(root, "sdk/modules/xiaotai_mod_loop_b/module.json", {"name": "loop_b", "depends": ["loop_a"]})
    write(root, "apps/xiaotai_app_companion/application.json",
          {"name": "companion", "modules": ["ai_talk"], "optional_modules": ["wake"], "start": True,
           "foreground": True, "page_step": True, "handles": ["ai_talk.state"], "claims": True})
    # D47: an application declares the activities it begins, like a module.
    write(root, "apps/xiaotai_app_alarm/application.json", {"name": "alarm", "needs": ["audio_out"],
                                                            "activities": {"ring": 3}})
    write(root, "apps/xiaotai_app_door/application.json", {"name": "door", "modules": ["lock"]})
    write(root, "apps/xiaotai_app_bad/application.json", {"name": "bad", "modules": ["loop_a"]})
    write(root, "apps/xiaotai_app_page_type/application.json", {"name": "page_type", "page_step": "true"})
    fails(lambda: selection.catalog(root), "page_step must be a boolean")
    write(root, "apps/xiaotai_app_page_type/application.json", {"name": "page_type", "page_step": False})

    units = selection.catalog(root)
    board = ["audio.pcm.capture", "audio.pcm.playback", "network.ip"]

    # Engines and modules in dependency order; optional modules only on request.
    apps, modules, components, _ = selection.resolve(units, ["companion", "alarm"], capabilities=board)
    assert [m.name for m in modules] == ["media", "platform", "transport", "provisioning", "binding", "ai_talk"], modules
    assert components[:3] == ["xiaotai_media", "xiaotai_platform", "xiaotai_transport"], components
    assert components[-2:] == ["xiaotai_app_companion", "xiaotai_app_alarm"], components
    apps, modules, _, _ = selection.resolve(units, ["companion"], ["wake"], board)
    names = [m.name for m in modules]
    assert names == ["media", "platform", "transport", "wake_engine",
                     "provisioning", "binding", "ai_talk", "wake"], names

    fails(lambda: selection.resolve(units, ["door"], capabilities=board), "lacks")
    fails(lambda: selection.resolve(units, ["bad"]), "cycle")
    fails(lambda: selection.resolve(units, ["nothing"]), "unknown application")
    fails(lambda: selection.resolve(units, [], ["absent"]), "does not exist")
    fails(lambda: selection.resolve(units, [], ["bad_engine"]), "must not depend on base module")

    # The generated tables drive the real framework: init order follows the manifests.
    apps, modules, _, _ = selection.resolve(units, ["companion", "alarm"], ["wake"], board)
    work = Path(temp) / "build"
    work.mkdir()
    (work / "selected.c").write_text(selection.generate(apps, modules), encoding="utf-8")
    stubs = ["#include <stdio.h>", "#include <string.h>", '#include "xiaotai/framework.h"',
             "char trace[512];", 'static void note(const char *s) { strcat(trace, s); strcat(trace, " "); }']
    for unit in modules:
        prefix = "xiaotai_engine" if unit.kind == "engine" else "xiaotai_module"
        stubs.append(f'xiaotai_err_t {prefix}_{unit.name}_init(void) {{ note("{unit.name}"); return XIAOTAI_OK; }}')
        if unit.start:
            stubs.append(f'void {prefix}_{unit.name}_start(void) {{ note("{unit.name}-start"); }}')
    for app in apps:
        stubs.append(f'xiaotai_err_t xiaotai_app_{app.name}_init(void) {{ note("app-{app.name}"); return XIAOTAI_OK; }}')
        if app.start:
            stubs.append(f'void xiaotai_app_{app.name}_start(void) {{ note("app-{app.name}-start"); }}')
        if app.foreground:
            stubs.append(f'void xiaotai_app_{app.name}_foreground(bool shown) {{ if (shown) note("shown-{app.name}"); }}')
        if app.page_step:
            stubs.append(f"xiaotai_err_t xiaotai_app_{app.name}_page_step(int direction) "
                         "{ return direction == -1 ? XIAOTAI_OK : XIAOTAI_ERR_BUSY; }")
        if app.claims:
            stubs.append(f"bool xiaotai_app_{app.name}_claim(const xiaotai_event_t *event) {{ (void)event; return true; }}")
    stubs.append("""
#include <unistd.h>
extern const xiaotai_module_t *const xiaotai_selected_modules[];
extern const size_t xiaotai_selected_module_count;
extern const xiaotai_application_t *const xiaotai_selected_applications[];
extern const size_t xiaotai_selected_application_count;
static volatile int probed;
static char arbitration[64];
/* On the owner task: "ai" ranks 2 by the ai_talk manifest, "room" is declared
 * by no unit and ranks 1, "ring" ranks 3 by the alarm application's manifest
 * (D47): it preempts the conversation, which then cannot begin over it. */
static void probe(void *ctx)
{
    (void)ctx;
    uint32_t ai = 0, room = 0;
    const xiaotai_activity_t talk = {.name = "ai"}, group = {.name = "room"};
    int a = xiaotai_activity_begin(&talk, &ai) == XIAOTAI_OK;
    int b = xiaotai_activity_begin(&group, &room) == XIAOTAI_ERR_BUSY;
    xiaotai_activity_end(ai);
    int c = xiaotai_activity_begin(&group, &room) == XIAOTAI_OK;
    int d = xiaotai_activity_begin(&talk, &ai) == XIAOTAI_OK;
    uint32_t alarm = 0;
    const xiaotai_activity_t ringing = {.name = "ring"};
    int e = xiaotai_activity_begin(&ringing, &alarm) == XIAOTAI_OK && xiaotai_activity_current(NULL) == alarm;
    int f = xiaotai_activity_begin(&talk, &ai) == XIAOTAI_ERR_BUSY;
    int g = xiaotai_app_page_step(-1) == XIAOTAI_OK && xiaotai_app_page_step(1) == XIAOTAI_ERR_BUSY;
    snprintf(arbitration, sizeof(arbitration), "%d%d%d%d%d%d%d", a, b, c, d, e, f, g);
    probed = 1;
}
int main(void)
{
    const xiaotai_framework_config_t config = {
        .modules = xiaotai_selected_modules, .module_count = xiaotai_selected_module_count,
        .applications = xiaotai_selected_applications, .application_count = xiaotai_selected_application_count,
        .available_needs = 0xffffffffu,
    };
    if (xiaotai_framework_start(&config) != XIAOTAI_OK) return 1;
    int optional = xiaotai_module_present("wake");
    if (xiaotai_run(probe, NULL) != XIAOTAI_OK) return 2;
    for (int i = 0; i < 2000 && !probed; ++i) usleep(1000);
    xiaotai_framework_reset();
    printf("%s|%d|%s\\n", trace, optional, arbitration);
    return 0;
}
""")
    (work / "main.c").write_text("\n".join(stubs), encoding="utf-8")
    sdk = ROOT / "sdk"
    sources = [work / "selected.c", work / "main.c",
               *sorted((sdk / "xiaotai_framework/src").glob("*.c")),
               *[sdk / "xiaotai_port/host" / f for f in ("os.c", "mem.c", "sys.c", "diag.c", "kv_memory.c")],
               *[sdk / "xiaotai_port/common" / f for f in ("kv_worker.c", "log_gate.c", "log_store.c")]]
    binary = work / "selected"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(sdk / "xiaotai_framework/include"), "-I", str(sdk / "xiaotai_framework/src"),
                    "-I", str(sdk / "xiaotai_framework/internal"),
                    "-I", str(sdk / "xiaotai_port/include"), "-I", str(sdk / "xiaotai_port/host/include"),
                    "-I", str(sdk / "xiaotai_port/common"),
                    *map(str, sources), "-o", str(binary)], check=True)
    out = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30,
                         env={**os.environ, "XIAOTAI_HOST_LOG_QUIET": "1"}).stdout.strip()
    trace, optional, arbitration = out.split("|")
    trace = trace.strip()
    assert trace == ("media platform transport wake_engine provisioning binding ai_talk wake "
                     "app-companion app-alarm media-start platform-start binding-start "
                     "app-companion-start shown-companion"), trace
    assert optional == "1"
    assert arbitration == "1111111", arbitration
    generated = selection.generate(apps, modules)
    assert '.activities = module_ai_talk_activities' in generated and '{"ai", 2},' in generated, generated
    assert ".handles = app_companion_handles, .claim = xiaotai_app_companion_claim" in generated, generated
    assert ".handles = NULL, .claim = NULL" in generated, generated
    assert ".activities = app_alarm_activities" in generated and '{"ring", 3},' in generated, generated
    assert ".page_step = xiaotai_app_companion_page_step" in generated and ".page_step = NULL" in generated, generated
    assert ".claim = xiaotai_app_companion_claim,\n    .activities = NULL" in generated, generated

    # Product generation: priorities, preemption rules, start-failure policy and
    # the board's declared needs, compiled against the real public headers.
    for name in selection.BASE_COMPONENTS:
        (root / "sdk" / name).mkdir(parents=True, exist_ok=True)
    board = {"parts": {"audio_input": [{}], "audio_output": [{}], "network": [{"kind": "wifi"}]},
             "memory": {"external_ram": True}}
    # The board's Wi-Fi part needs its driver: the engine declaring the kind.
    write(root, "sdk/xiaotai_net/engine.json", {"name": "net", "network_kinds": ["wifi", "wifi_coprocessor"]})
    product = {"schema": 1, "name": "demo", "board": "acme/one", "applications": ["companion"],
               "modules": ["wake"], "priorities": {"ai": 3},
               "preemptions": [{"activity": "ai", "over": "ai", "preempts": True}]}
    path = root / "products/demo/product.json"
    write(root, "products/demo/product.json", product)
    fails(lambda: selection.load_product(path), "two different activities")
    product["preemptions"] = [{"activity": "ai", "over": "room", "preempts": False}]
    product["start_failure_restart_ms"] = -2
    write(root, "products/demo/product.json", product)
    fails(lambda: selection.load_product(path), "start_failure_restart_ms")
    product["start_failure_restart_ms"] = -1
    write(root, "products/demo/product.json", product)
    loaded = selection.load_product(path)
    fails(lambda: selection.plan(root, loaded, board), "activity room is begun by no selected unit")
    product["preemptions"] = []
    write(root, "products/demo/product.json", product)
    loaded = selection.load_product(path)
    result = selection.plan(root, loaded, board)
    assert result["activities"] == {"ai": 2}, result["activities"]
    assert result["network_drivers"] == ["net"] and "net" in result["modules"], result
    assert result["components"].keys() >= {"xiaotai_json", "xiaotai_sdk"}, result["components"]
    apps, modules, board = result["units"]
    loaded["preemptions"] = [{"activity": "ai", "over": "call", "preempts": True}]
    text = selection.generate(apps, modules, loaded, board)
    mask = selection.needs_mask({"audio_in", "pcm_in", "audio_out", "network", "external_ram"})
    assert f"const uint32_t xiaotai_board_declared_needs = 0x{mask:x}u;" in text, text
    assert '{"ai", "call", true},' in text and ".start_failure_restart_ms = XIAOTAI_START_HALT" in text, text
    (work / "product.c").write_text(text, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-c",
                    "-I", str(sdk / "xiaotai_framework/include"), "-I", str(sdk / "xiaotai_port/include"),
                    "-I", str(sdk / "xiaotai_port/host/include"), "-I", str(sdk / "xiaotai_sdk/include"),
                    str(work / "product.c"), "-o", str(work / "product.o")], check=True)
    fails(lambda: selection.generate(apps, modules, loaded, {}), "no \"parts\"")

    # Manifest fields: activity conflicts, handles, claims.
    write(root, "sdk/modules/xiaotai_mod_room/module.json", {"name": "room", "activities": {"ai": 1}})
    fails(lambda: selection.resolve(selection.catalog(root), ["companion"], ["room"]), "activity ai")
    write(root, "sdk/modules/xiaotai_mod_room/module.json", {"name": "room", "activities": {"room": 0}})
    fails(lambda: selection.catalog(root), "priority 1..255")
    write(root, "sdk/modules/xiaotai_mod_room/module.json", {"name": "room"})
    write(root, "apps/xiaotai_app_alarm/application.json", {"name": "alarm", "claims": True})
    fails(lambda: selection.catalog(root), "claims without handles")
    write(root, "apps/xiaotai_app_alarm/application.json", {"name": "alarm", "handles": ["clock.alarm"]})
    fails(lambda: selection.catalog(root), "no unit named clock")

    # Manifest "commands" (xiaotai_at.md 4.0): checked and carried.
    write(root, "apps/xiaotai_app_alarm/application.json", {"name": "alarm", "commands": [
        {"command": "AT+ALARM", "verb": "SET", "usage": "<hh:mm>", "access": "action"},
        {"command": "AT+ALARM", "verb": "WIPE", "access": "guarded", "option": "XIAOTAI_ALARM_DEV",
         "symbol": "alarm_dev_"}]})
    assert len(selection.catalog(root)["application"]["alarm"].commands) == 2
    write(root, "apps/xiaotai_app_alarm/application.json", {"name": "alarm", "commands": [
        {"command": "AT+alarm", "access": "read"}]})
    fails(lambda: selection.catalog(root), "bad command")
    write(root, "apps/xiaotai_app_alarm/application.json", {"name": "alarm", "commands": [
        {"command": "AT+ALARM", "access": "read", "option": "XIAOTAI_ALARM_DEV"}]})
    fails(lambda: selection.catalog(root), "go together")
    write(root, "apps/xiaotai_app_alarm/application.json", {"name": "alarm", "needs": ["audio_out"]})

# D47 over the catalog: an application and a module giving one activity
# different defaults is an error; the product may rank an application's activity.
with tempfile.TemporaryDirectory(prefix="xiaotai-selection-apps-") as temp:
    root = Path(temp)
    write(root, "sdk/modules/xiaotai_mod_call/module.json", {"name": "call", "activities": {"call": 3}})
    write(root, "apps/xiaotai_app_alarm/application.json",
          {"name": "alarm", "modules": ["call"], "activities": {"call": 2}})
    fails(lambda: selection.resolve(selection.catalog(root), ["alarm"]), "activity call")
    write(root, "apps/xiaotai_app_alarm/application.json",
          {"name": "alarm", "modules": ["call"], "activities": {"ring": 0}})
    fails(lambda: selection.catalog(root), "priority 1..255")
    write(root, "apps/xiaotai_app_alarm/application.json",
          {"name": "alarm", "modules": ["call"], "activities": {"ring": 4}})
    for name in selection.BASE_COMPONENTS:
        (root / "sdk" / name).mkdir(parents=True, exist_ok=True)
    product = {"schema": 1, "name": "clock", "board": "acme/one", "applications": ["alarm"],
               "priorities": {"ring": 2}, "preemptions": [{"activity": "call", "over": "ring", "preempts": False}]}
    result = selection.plan(root, product, {})
    assert result["activities"] == {"call": 3, "ring": 4}, result["activities"]

# Board packages: role families, the needs.json spellings, network drivers.
# The real door_lock rule (needs.json, bit 16): a relay of the role family
# "door" matches "door" and "door.<anything>", never a name that merely starts
# with it; the plain relay need stays independent of the role.
door_lock = next(rule for rule in selection.NEED_RULES if rule["name"] == "door_lock")
assert door_lock["bit"] == 16 and door_lock["where"] == {"kind": ["relay"], "role": ["door"]}, door_lock
assert selection.NEEDS["door_lock"] == 1 << 16


def lock(output):
    return "door_lock" in selection.board_needs({"parts": {"output": output}})


assert lock([{"kind": "relay", "role": "door"}])
assert lock([{"kind": "light"}, {"kind": "relay", "role": "door.gate"}])
assert not lock([{"kind": "relay", "role": "doorbell"}])
assert not lock([{"kind": "relay"}])
assert not lock([{"kind": "light", "role": "door"}])
assert "relay" in selection.board_needs({"parts": {"output": [{"kind": "relay", "role": "doorbell"}]}})

good = {"parts": {"input": [{"kind": "button"}, {"kind": "slider"}],
                  "network": [{"kind": "wifi_coprocessor", "wifi": {"tx_buffers_on_demand": True}}],
                  "measurement": [{"kind": "temperature"}], "output": [{"kind": "relay", "role": "door.gate"}]}}
selection.check_board(good)
assert selection.board_needs(good) >= {"key", "relay", "network"}, selection.board_needs(good)
assert not selection.board_needs(good) & {"light", "encoder", "display"}, selection.board_needs(good)
for parts, message in (({"input": [{"kind": "knob"}]}, "input[0].kind"),
                       ({"measurement": [{"kind": "pressure"}]}, "measurement[0].kind"),
                       ({"network": [{"kind": "wifi", "wifi": True}]}, "wifi must be an object"),
                       ({"network": [{}]}, "names no kind"),
                       ({"network": [{"kind": "lora"}]}, "network[0].kind"),
                       ({"output": [{"kind": "relay", "role": "Door"}]}, "role must be"),
                       ({"input": "button"}, "must be a list"),
                       # A list or object where a spelling belongs is a spelling
                       # fault naming the field, not a Python TypeError.
                       ({"network": [{"kind": ["wifi"]}]}, "network[0].kind is ['wifi']"),
                       ({"display": [{"pixel": {"rgb565": True}}]}, "display[0].pixel"),
                       ({"input": [{"kind": 3}]}, "input[0].kind is 3")):
    fails(lambda: selection.check_board({"parts": parts}), message)

with tempfile.TemporaryDirectory(prefix="xiaotai-selection-net-") as temp:
    root = Path(temp)
    for name in selection.BASE_COMPONENTS:
        (root / "sdk" / name).mkdir(parents=True, exist_ok=True)
    write(root, "sdk/xiaotai_net/engine.json", {"name": "net", "network_kinds": ["wifi", "wifi_coprocessor"]})
    write(root, "sdk/xiaotai_eth/engine.json", {"name": "eth", "depends": ["net"], "network_kinds": ["ethernet"]})
    write(root, "sdk/modules/xiaotai_mod_provisioning/module.json",
          {"name": "provisioning", "depends": ["net"], "needs": ["network"]})
    write(root, "apps/xiaotai_app_hub/application.json", {"name": "hub", "modules": ["provisioning"]})
    write(root, "apps/xiaotai_app_clock/application.json", {"name": "clock"})
    hub = {"schema": 1, "name": "hub", "board": "acme/one", "applications": ["hub"]}
    wired = {"parts": {"network": [{"kind": "ethernet"}, {"kind": "wifi"}]}}
    # The board's Ethernet part selects its driver, initialized after net.
    result = selection.plan(root, hub, wired)
    assert result["network_drivers"] == ["eth", "net"], result["network_drivers"]
    assert result["modules"] == ["net", "eth", "provisioning"], result["modules"]
    # A product that does not use the network selects no driver.
    clock = {"schema": 1, "name": "clock", "board": "acme/one", "applications": ["clock"]}
    assert selection.plan(root, clock, wired)["modules"] == []
    fails(lambda: selection.plan(root, hub, {"parts": {"network": [{"kind": "cellular"}]}}), "has no driver")
    write(root, "sdk/xiaotai_eth_b/engine.json", {"name": "eth_b", "depends": ["net"],
                                                  "network_kinds": ["ethernet"]})
    fails(lambda: selection.plan(root, hub, wired), "must select one of them")
    chosen = dict(hub, modules=["eth_b"])
    assert selection.plan(root, chosen, wired)["network_drivers"] == ["eth_b", "net"]
    write(root, "sdk/xiaotai_eth_b/engine.json", {"name": "eth_b", "network_kinds": ["token_ring"]})
    fails(lambda: selection.catalog(root), "not network part kinds")
    write(root, "sdk/xiaotai_eth_b/engine.json", {"name": "eth_b", "network_kinds": ["ethernet", "ethernet"]})
    fails(lambda: selection.catalog(root), "distinct network part kinds")

# Themes named in the parameters, and the product's AT vocabulary.
with tempfile.TemporaryDirectory(prefix="xiaotai-selection-theme-") as temp:
    root = Path(temp)
    for name in selection.BASE_COMPONENTS:
        (root / "sdk" / name).mkdir(parents=True, exist_ok=True)
    write(root, "sdk/xiaotai_ui/engine.json", {
        "name": "ui", "needs": ["display"],
        "configure": [{"function": "xiaotai_ui_configure", "header": "xiaotai/ui.h", "type": "xiaotai_ui_config_t",
                       "overrides": {"theme": "theme"}}],
        "commands": [{"command": "AT+UI", "access": "read"},
                     {"command": "AT+UI", "usage": "<page>", "access": "action"},
                     {"command": "AT+UI", "verb": "SLEEP", "access": "action"}]})
    write(root, "sdk/themes/xiaotai_theme_small/theme.json", {
        "name": "theme_small", "depends": ["ui"], "display": {"pixel": ["mono1"], "width": [128, 128]}})
    write(root, "sdk/themes/xiaotai_theme_big/theme.json", {
        "name": "theme_big", "depends": ["ui"], "display": {"pixel": ["rgb565"], "width": [320, 1024]}})
    write(root, "sdk/modules/xiaotai_mod_call/module.json", {"name": "call", "commands": [
        {"command": "AT+CALL", "verb": "ACCEPT", "access": "action"},
        {"command": "AT+CALL", "verb": "DEVICE", "usage": "<id>", "access": "action"}]})
    write(root, "sdk/modules/xiaotai_mod_wechat/module.json", {"name": "wechat", "commands": [
        {"command": "AT+CALL", "verb": "ACCEPT", "access": "action"}]})
    write(root, "apps/xiaotai_app_panel/application.json", {"name": "panel", "modules": ["ui", "call", "wechat"],
                                                            "commands": [{"command": "AT+CALL", "verb": "DISMISS",
                                                                          "access": "action"}]})
    mono = {"parts": {"display": [{"pixel": "mono1", "width": 128, "height": 64}]}}
    panel = {"schema": 1, "name": "panel", "board": "acme/one", "applications": ["panel"],
             "modules": ["theme_small"], "parameters": {"ui": {"theme": "small"}}}
    result = selection.plan(root, panel, mono)
    single = {"parts": {"display": mono["parts"]["display"][0]}}
    assert selection.plan(root, panel, single)["commands"] == result["commands"]
    incompatible = {"parts": {"display": {"pixel": "rgb565", "width": 480, "height": 320}}}
    fails(lambda: selection.plan(root, panel, incompatible), "draws for none")
    fails(lambda: selection.plan(root, dict(panel, parameters={"ui": {"theme": "big"}}), mono),
          "selects no theme unit theme_big")
    color = {"parts": {"display": [{"pixel": "rgb565", "width": 480, "height": 320},
                                   {"pixel": "mono1", "width": 128, "height": 64}]}}
    both = dict(panel, modules=["theme_small", "theme_big"])
    selection.plan(root, dict(both, parameters={"ui": {"theme": "big"}}), color)
    fails(lambda: selection.plan(root, both, color), "does not draw for the board's first display")
    selection.plan(root, dict(both, parameters={"ui": {"theme": None}}), color)
    # Vocabulary: one entry per spelling (command, verb, argument form), with
    # every unit that registers it; a bare query and a bare write of one
    # command are two spellings with their own access.
    vocabulary = {(entry["command"], entry["verb"], entry["usage"]): entry for entry in result["commands"]}
    assert list(vocabulary) == [("AT+CALL", "ACCEPT", ""), ("AT+CALL", "DEVICE", "<id>"),
                                ("AT+CALL", "DISMISS", ""), ("AT+UI", None, ""), ("AT+UI", None, "<page>"),
                                ("AT+UI", "SLEEP", "")], list(vocabulary)
    assert vocabulary[("AT+CALL", "ACCEPT", "")]["units"] == ["call", "wechat"], vocabulary
    assert vocabulary[("AT+CALL", "DISMISS", "")]["units"] == ["app.panel"], vocabulary
    assert [vocabulary[("AT+UI", None, form)]["access"] for form in ("", "<page>")] == ["read", "action"]
    out = root / "generated"
    selection.write(root, panel, out, mono)
    written = json.loads((out / "commands.json").read_text(encoding="utf-8"))
    assert written == {"product": "panel", "commands": result["commands"]}, written
    write(root, "sdk/modules/xiaotai_mod_wechat/module.json", {"name": "wechat", "commands": [
        {"command": "AT+CALL", "verb": "ACCEPT", "access": "guarded"}]})
    fails(lambda: selection.plan(root, panel, mono), "AT+CALL ACCEPT: module wechat declares access guarded")
    # Same access, but one declaration is compiled only under a build option.
    write(root, "sdk/modules/xiaotai_mod_wechat/module.json", {"name": "wechat", "commands": [
        {"command": "AT+CALL", "verb": "ACCEPT", "access": "action", "option": "XIAOTAI_WECHAT_DEV",
         "symbol": "wechat_dev_"}]})
    fails(lambda: selection.plan(root, panel, mono),
          "AT+CALL ACCEPT: module wechat declares access action option XIAOTAI_WECHAT_DEV, call declares "
          "access action option None")
    write(root, "sdk/modules/xiaotai_mod_wechat/module.json", {"name": "wechat", "commands": [
        {"command": "AT+CALL", "verb": "ACCEPT", "access": "action"}]})
    # The bare AT is the console engine's syntax: in the vocabulary exactly
    # when "at" is selected, first in order, and no manifest declares it.
    assert not any(entry["command"] == "AT" for entry in result["commands"]), result["commands"]
    write(root, "sdk/xiaotai_at/engine.json", {"name": "at", "commands": [
        {"command": "AT+HELP", "access": "read"}]})
    words = selection.plan(root, panel, mono)["commands"]
    assert words[0] == {"command": "AT", "verb": None, "usage": "", "access": "read", "option": None,
                        "symbol": None, "units": ["at"]}, words[0]
    assert [entry["command"] for entry in words].count("AT") == 1 and words[1]["command"] == "AT+CALL", words
    # A theme whose display rule misspells a pixel format fails when read.
    write(root, "sdk/themes/xiaotai_theme_small/theme.json", {
        "name": "theme_small", "depends": ["ui"], "display": {"pixel": ["mono"], "width": [128, 128]}})
    fails(lambda: selection.catalog(root), "display.pixel ['mono'] are not pixel formats of board.json")
    write(root, "sdk/themes/xiaotai_theme_small/theme.json", {
        "name": "theme_small", "depends": ["ui"], "display": {"pixel": ["mono1"], "width": [128, 128]}})
    # --check: an activity an application header defines must be declared in
    # its manifest, or the framework would rank it 1 whatever the default.
    header = root / "apps/xiaotai_app_panel/include/xiaotai/apps/panel.h"
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text('#define XIAOTAI_APP_PANEL_ACTIVITY "panel_hold"\n', encoding="utf-8")
    assert selection.missing_activities(root) == [("apps/xiaotai_app_panel", "panel_hold")], \
        selection.missing_activities(root)
    write(root, "apps/xiaotai_app_panel/application.json", {
        "name": "panel", "modules": ["ui", "call", "wechat"], "activities": {"panel_hold": 1},
        "commands": [{"command": "AT+CALL", "verb": "DISMISS", "access": "action"}]})
    assert selection.missing_activities(root) == [], selection.missing_activities(root)
    # configure "absent_with": a field another selected unit owns is refused.
    write(root, "sdk/modules/xiaotai_mod_clock/module.json", {"name": "clock", "configure": [
        {"function": "xiaotai_clock_module_configure", "header": "xiaotai/modules/clock.h",
         "type": "xiaotai_clock_module_config_t", "absent_with": {"call": ["timezone", "time_servers"]}}]})
    timed = dict(panel, modules=["theme_small", "clock"],
                 parameters={"ui": {"theme": "small"}, "clock": {"timezone": "CST-8", "timezone_setting": True}})
    fails(lambda: selection.plan(root, timed, mono), "parameters.clock: ['timezone'] must not be given when the "
                                                     "product selects call")
    apps, modules, _ = selection.plan(root, dict(timed, parameters={"ui": {"theme": "small"}}), mono)["units"]
    fails(lambda: selection.generate(apps, modules, timed, mono), "must not be given when the product selects call")
    selection.plan(root, dict(timed, parameters={"clock": {"timezone_setting": True}}), mono)
    write(root, "sdk/modules/xiaotai_mod_clock/module.json", {"name": "clock", "depends": [], "configure": [
        {"function": "xiaotai_clock_module_configure", "header": "xiaotai/modules/clock.h",
         "type": "xiaotai_clock_module_config_t", "absent_with": {"call": []}}]})
    fails(lambda: selection.catalog(root), "absent_with maps unit names to lists of parameter fields")
    write(root, "sdk/modules/xiaotai_mod_clock/module.json", {"name": "clock"})

# Base engines: eventlog, at and identity first, in this order, whatever selects them.
with tempfile.TemporaryDirectory(prefix="xiaotai-selection-base-") as temp:
    root = Path(temp)
    write(root, "sdk/xiaotai_zeta/engine.json", {"name": "zeta"})
    write(root, "sdk/xiaotai_identity/engine.json", {"name": "identity"})
    write(root, "sdk/xiaotai_at/engine.json", {"name": "at", "depends": ["eventlog"]})
    write(root, "sdk/xiaotai_eventlog/engine.json", {"name": "eventlog"})
    write(root, "sdk/modules/xiaotai_mod_clock/module.json", {"name": "clock", "depends": ["zeta", "at"]})
    write(root, "apps/xiaotai_app_alarm/application.json", {"name": "alarm", "modules": ["clock"]})
    _, modules, _, _ = selection.resolve(selection.catalog(root), ["alarm"])
    assert [m.name for m in modules] == ["eventlog", "at", "identity", "zeta", "clock"], modules
    write(root, "sdk/xiaotai_identity/engine.json", {"name": "identity", "depends": ["zeta"]})
    fails(lambda: selection.resolve(selection.catalog(root), ["alarm"]), "base engines")

# Needs of a board package by the rules of sdk/xiaotai_hal/needs.json.
assert selection.board_needs({"parts": {
    "audio_input": [{"encoding": "opus"}], "audio_output": [{}],
    "display": [{"pixel": "mono1", "width": 128, "height": 64}],
    "input": [{"kind": "button"}, {"kind": "contact"}], "output": [{"kind": "relay"}],
    "network": [{"kind": "wifi"}]}, "memory": {"external_ram": False}}) == {
    "audio_in", "audio_out", "display", "key", "relay", "network"} | ({"contact"} & set(selection.NEEDS))
assert selection.board_needs({"parts": {"audio_input": [{}], "display": [{"touch": True}],
                                        "output": [{"kind": "rgb_light"}]}}) == {
    "audio_in", "pcm_in", "display", "touch", "light"}

touch_product = selection.load_product(ROOT / "products/xiaotai-szpi-s3/product.json")
touch_board = json.loads((ROOT / "boards" / touch_product["board"] / "board.json").read_text(encoding="utf-8"))
touch_only = dict(touch_board, parts=dict(touch_board["parts"], input=[]))
assert set(selection.plan(ROOT, touch_product, touch_only)["needs"]) & {"key", "touch"} == {"touch"}

# The stock interaction contract belongs to the product, not the theme or
# Companion extensions. A non-touch OLED still needs its main button. The
# single-key C3 product is the fixture; a tree without it skips these checks.
if product_scope.has_product(product_scope.XMINI):
    key_product = selection.load_product(ROOT / "products/xiaotai-xmini-c3-v3/product.json")
    key_board = json.loads((ROOT / "boards" / key_product["board"] / "board.json").read_text(encoding="utf-8"))
    no_input = dict(key_board, parts=dict(key_board["parts"], input=[]))
    assert {"display", "key", "pcm_in"} <= set(selection.plan(ROOT, key_product, key_board)["needs"])
    with tempfile.TemporaryDirectory(prefix="xiaotai-interaction-") as temp:
        generated = Path(temp) / "out"
        fails(lambda: selection.write(ROOT, key_product, generated, no_input),
              "needs one of ['key', 'touch'], which this board lacks")
        assert not generated.exists(), generated
        for invalid in (None, [], "key", ["key", "key"], ["missing"], [{"key": True}]):
            for requirement in ("needs_any", "needs_all"):
                write(Path(temp), "product.json", dict(key_product, **{requirement: invalid}))
                fails(lambda: selection.load_product(Path(temp) / "product.json"), f"{requirement} must be")

    # The stock Xmini capture contract is additional to key or touch. An actual
    # encoded microphone can provide audio_in without satisfying pcm_in.
    encoded_mic = {"encoding": "opus", "rate_hz": 16000, "roles": ["mic"], "frame_ms": 20}
    encoded_board = dict(key_board, parts=dict(key_board["parts"], audio_input=[encoded_mic]))
    with tempfile.TemporaryDirectory(prefix="xiaotai-product-pcm-") as temp:
        generated = Path(temp) / "out"
        fails(lambda: selection.write(ROOT, key_product, generated, encoded_board),
              "needs ['pcm_in'], which this board lacks")
        assert not generated.exists(), generated
        # A product may still select the same modules on encoded hardware.
        encoded_product = {name: value for name, value in key_product.items() if name != "needs_all"}
        available = set(selection.write(ROOT, encoded_product, generated, encoded_board)["needs"])
        assert {"audio_in", "key"} <= available and "pcm_in" not in available
        fails(lambda: selection.plan(ROOT, dict(encoded_product, needs_all=["audio_in", "pcm_in"]), encoded_board),
              "needs ['pcm_in'], which this board lacks")
        only_all = {name: value for name, value in key_product.items() if name != "needs_any"}
        fails(lambda: selection.plan(ROOT, only_all, {}), "needs_all requires board parts")

    # A product that uses the plain style has no UI module, theme or UI options.
    headless = dict(key_board, parts={name: parts for name, parts in key_board["parts"].items() if name != "display"})
    plain = {"name": "companion-headless", "board": key_product["board"], "applications": ["companion"],
             "needs_any": key_product["needs_any"]}
    assert set(selection.plan(ROOT, plain, headless)["needs"]) & {"display", "key", "touch"} == {"key"}

    # An encoder style is supplied by custom code, not by the stock contract.
    # The separate app_companion_style test proves that it works at runtime.
    encoder_only = dict(no_input, parts=dict(no_input["parts"], input=[{"kind": "encoder", "role": "main"}]))
    custom = {name: value for name, value in key_product.items() if name != "needs_any"}
    assert set(selection.plan(ROOT, custom, encoder_only)["needs"]) & {"encoder", "key", "touch"} == {"encoder"}
    assert "encoder" in selection.plan(ROOT, dict(custom, needs_any=["encoder"]), encoder_only)["needs"]
    fails(lambda: selection.plan(ROOT, key_product, {}), "needs_any requires board parts")

# The real catalog: the commands the applications register reach the
# product's vocabulary (what the bench dialect, the console script's
# permissions and the command list are generated from).
station = {"parts": {"audio_input": [{"encoding": "pcm16"}], "audio_output": [{}],
                     "display": [{"pixel": "rgb565", "width": 1024, "height": 600, "touch": True}],
                     "network": [{"kind": "wifi_coprocessor", "wifi": {}}]},
           "memory": {"external_ram": True}}
both = {"schema": 1, "name": "station", "board": "acme/station", "applications": ["door", "companion"],
        "modules": ["theme_lcd480", "call"]}
real = {(entry["command"], entry["verb"], entry["usage"]): entry
        for entry in selection.plan(ROOT, both, station)["commands"]}
assert real[("AT", None, "")]["units"] == ["at"], real[("AT", None, "")]
assert real[("AT+CALL", "DISMISS", "")]["units"] == ["app.companion"], real[("AT+CALL", "DISMISS", "")]
assert real[("AT+CALL", "DISMISS", "")]["access"] == "action"
door_access = {verb: real[("AT+DOOR", verb, "")]["access"]
               for verb in (None, "HOME", "NEXT", "PREVIOUS", "VIEWER", "SENDER", "DEVICES", "BACK")}
assert door_access == {None: "read", "HOME": "action", "NEXT": "action", "PREVIOUS": "action",
                       "VIEWER": "guarded", "SENDER": "guarded", "DEVICES": "action", "BACK": "action"}, door_access
assert all(real[("AT+DOOR", verb, "")]["units"] == ["app.door"] for verb in door_access)
alone = selection.plan(ROOT, dict(both, applications=["companion"]), station)["commands"]
assert any(entry["command"] == "AT+CALL" and entry["verb"] == "DISMISS" for entry in alone)
assert not any(entry["command"] == "AT+DOOR" for entry in alone), alone
# XR-DOORLOCK in the real tree: the lock module declares door_lock, so the
# door application's optional lock selects only on a board with a relay of the
# role family "door"; any other relay satisfies relay but not the lock.
lock_manifest = json.loads((ROOT / "sdk/modules/xiaotai_mod_lock/module.json").read_text(encoding="utf-8"))
assert lock_manifest["needs"] == ["door_lock"], lock_manifest
locked = dict(both, modules=both["modules"] + ["lock"])


def station_with(output):
    return dict(station, parts=dict(station["parts"], output=output))


for output in ([], [{"kind": "relay", "role": "doorbell"}], [{"kind": "relay"}], [{"kind": "light", "role": "door"}]):
    fails(lambda: selection.plan(ROOT, locked, station_with(output)),
          "module lock needs ['door_lock'], which this board lacks")
for role in ("door", "door.gate"):
    chosen = selection.plan(ROOT, locked, station_with([{"kind": "relay", "role": "doorbell"},
                                                        {"kind": "relay", "role": role}]))
    assert "lock" in chosen["modules"], chosen["modules"]
    assert ("AT+LOCK", None, "") in {(e["command"], e["verb"], e["usage"]) for e in chosen["commands"]}
# Without the lock the same door product needs no door relay.
assert "lock" not in selection.plan(ROOT, both, station_with([]))["modules"]

# XR-CODEC in the real tree: board.json audio encodings are the audio values of
# the one codec vocabulary (sdk/xiaotai_port/include/xiaotai/codec.h), spelled
# as the enumerator without XIAOTAI_CODEC_ in lower case, so a value appended
# there needs its spelling here and nothing converts between lists. Only pcm16
# derives pcm_in (an encoded serial capture never stands in for PCM); a part
# that gives no encoding takes pcm16 from the manifest defaults only.
codec_header = (ROOT / "sdk/xiaotai_port/include/xiaotai/codec.h").read_text(encoding="utf-8")
codec_values = {name: int(value) for name, value in re.findall(r"XIAOTAI_CODEC_([A-Z0-9_]+)\s*=\s*(\d+)",
                                                                   codec_header)}
audio_macro = re.search(r"#define XIAOTAI_CODEC_IS_AUDIO\(codec\)(.*?)\n(?!\s)", codec_header, re.S).group(1)
audio_codecs = set(re.findall(r"XIAOTAI_CODEC_([A-Z0-9_]+)", audio_macro))
assert codec_values["NONE"] == 0 and codec_values["PCM16"] == 5 and {"PCM16", "OPUS", "G711A"} <= audio_codecs
assert not audio_codecs & {"NONE", "JPEG", "H264"}, audio_codecs
encoding_words = selection.PART_VOCABULARY["audio_input.encoding"]
assert encoding_words == {name.lower(): f"XIAOTAI_CODEC_{name}" for name in audio_codecs}, encoding_words


def capture(part):
    return {"parts": {"audio_input": [part], "audio_output": [{}]}}


for word in encoding_words:
    selection.check_board(capture({"encoding": word}))
    assert ("pcm_in" in selection.board_needs(capture({"encoding": word}))) == (word == "pcm16"), word
assert "pcm_in" in selection.board_needs(capture({"rate_hz": 16000}))
for word in [name.lower() for name in codec_values if name not in audio_codecs] + ["pcm", "PCM16", "g711"]:
    fails(lambda: selection.check_board(capture({"encoding": word})), "parts.audio_input[0].encoding")
# The wake engine needs PCM: a board whose only capture delivers Opus from a
# voice chip cannot select it, the same product on a PCM capture can.
waking = dict(both, applications=["companion"], modules=["theme_lcd480", "wake"])
opus_station = dict(station, parts=dict(station["parts"], audio_input=[{"encoding": "opus", "rate_hz": 16000}]))
fails(lambda: selection.plan(ROOT, waking, opus_station), "needs ['pcm_in'], which this board lacks")
assert "wake" in selection.plan(ROOT, waking, station)["modules"]

# The real clock module refuses its own time zone and time servers wherever
# the platform core keeps time (every product with a companion selects it).
timed = {"schema": 1, "name": "timed", "board": "acme/one", "applications": ["companion"], "modules": ["clock"]}
selection.plan(ROOT, dict(timed, parameters={"clock": {"sync_retry_s": 60}}), {})
fails(lambda: selection.plan(ROOT, dict(timed, parameters={"clock": {"time_servers": ["a"]}}), {}),
      "['time_servers'] must not be given when the product selects platform")

# Optional edges order selected units, but never select an optional unit.
with tempfile.TemporaryDirectory(prefix="xiaotai-selection-options-") as temp:
    root = Path(temp)
    write(root, "sdk/modules/video_call/module.json",
          {"name": "video_call", "optional_depends": ["call", "wechat"], "one_of": [["call", "wechat"]]})
    write(root, "sdk/modules/call/module.json", {"name": "call"})
    write(root, "sdk/modules/wechat/module.json", {"name": "wechat"})
    units = selection.catalog(root)
    fails(lambda: selection.resolve(units, [], ["video_call"]), "needs one of ['call', 'wechat']")
    for voice in ("call", "wechat"):
        _, selected, _, _ = selection.resolve(units, [], ["video_call", voice])
        assert [unit.name for unit in selected] == [voice, "video_call"]
    write(root, "sdk/modules/call/module.json", {"name": "call", "optional_depends": ["video_call"]})
    fails(lambda: selection.resolve(selection.catalog(root), [], ["video_call", "call"]), "dependency cycle")


with tempfile.TemporaryDirectory(prefix="xiaotai-selection-registrations-") as temp:
    root = Path(temp)
    for component in selection.BASE_COMPONENTS:
        (root / "sdk" / component).mkdir(parents=True)
    (root / "sdk/xiaotai_framework/Kconfig").write_text(
        "config XIAOTAI_FRAMEWORK_MAX_SETTINGS\nconfig XIAOTAI_FRAMEWORK_MAX_COMMANDS\n", encoding="utf-8")
    app = {"name": "panel", "optional_modules": ["call", "wechat"], "registrations": {
        "intents": 16, "subscribers": 18, "settings": 6, "commands": 6, "when_selected": [
            {"any_of": ["call", "wechat"], "intents": 2, "settings": 2, "commands": 2},
            {"any_of": ["wechat"], "subscribers": 3, "settings": 1, "commands": 2},
        ],
    }}
    app_path = "apps/panel/application.json"
    write(root, app_path, app)
    write(root, "sdk/modules/call/module.json", {"name": "call", "registrations": {"intents": 4, "commands": 1}})
    write(root, "sdk/modules/wechat/module.json",
          {"name": "wechat", "registrations": {"subscribers": 5, "settings": 1, "commands": 1}})
    product = {"schema": 1, "name": "demo", "board": "acme/one", "applications": ["panel"],
               "modules": ["call", "wechat"], "variants": {"lite": {"without": ["wechat"]}}}
    reduced = selection.apply_variant(product, "lite")

    def options(config, external=None):
        board = {} if external is None else {"memory": {"external_ram": external}}
        return selection.plan(root, config, board)["options"]

    # Include the built-in restart intent. Count a match of both optional
    # modules once, a single match once, and an absent group not at all.
    assert options(product) == ["CONFIG_XIAOTAI_FRAMEWORK_MAX_INTENTS=23",
                                "CONFIG_XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS=26"]
    assert options(reduced) == ["CONFIG_XIAOTAI_FRAMEWORK_MAX_INTENTS=23",
                                "CONFIG_XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS=18"]
    assert options(dict(product, modules=[])) == ["CONFIG_XIAOTAI_FRAMEWORK_MAX_INTENTS=17",
                                                 "CONFIG_XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS=18"]
    settings = "CONFIG_XIAOTAI_FRAMEWORK_MAX_SETTINGS="
    commands = "CONFIG_XIAOTAI_FRAMEWORK_MAX_COMMANDS="
    assert options(product, True) == options(product)
    assert options(product, False) == options(product) + [settings + "10", commands + "12"]
    assert options(reduced, False)[-2:] == [settings + "8", commands + "9"]
    assert options(dict(product, modules=[]), False)[-2:] == [settings + "8", commands + "8"]
    manual = dict(product, options={"FRAMEWORK_MAX_SETTINGS": 64})
    assert [line for line in options(manual, False) if line.startswith(settings)] == [settings + "64"]
    manual = dict(product, options={"FRAMEWORK_MAX_COMMANDS": 72})
    for memory in (None, False, True):
        assert [line for line in options(manual, memory) if line.startswith(commands)] == [commands + "72"]
    bad = {"FRAMEWORK_MAX_INTENTS": 99}
    fails(lambda: options(dict(product, options=bad)), "is generated from selected unit registrations")
    write(root, "product.json", dict(product, variants={"bad": {"options": bad}}))
    fails(lambda: selection.load_product(root / "product.json"), "variant bad: FRAMEWORK_MAX_INTENTS is generated")
    write(root, app_path, dict(app, options={"FRAMEWORK_MAX_SUBSCRIBERS": 16}))
    fails(lambda: options(product), "FRAMEWORK_MAX_SUBSCRIBERS is generated")
    for value in (-1, True, "4", 1025):
        write(root, app_path, dict(app, registrations={"intents": value}))
        fails(lambda: options(product), "registrations.intents must be an integer")
    for group in ([], ["missing"], ["call", "call"]):
        write(root, app_path, dict(app, registrations={"when_selected": [{"any_of": group, "intents": 4}]}))
        fails(lambda: options(product), "any_of needs distinct declared optional units")
    write(root, app_path, dict(app, registrations={"intents": 1024}))
    fails(lambda: options(product), "selected units register 1029, maximum is 1024")
    for value in (-1, False, "4", 513):
        write(root, app_path, dict(app, registrations={"settings": value}))
        fails(lambda: options(product), "registrations.settings must be an integer from 0 to 512")
    write(root, app_path, dict(app, registrations={"settings": 512}))
    assert settings + "512" in options(dict(product, modules=[]), False)
    fails(lambda: options(product, False), "selected units register 513, maximum is 512")
    for value in (-1, True, "4", 513):
        write(root, app_path, dict(app, registrations={"commands": value}))
        fails(lambda: options(product), "registrations.commands must be an integer from 0 to 512")
    write(root, app_path, dict(app, registrations={"commands": 512}))
    assert options(dict(product, modules=[]), False)[-1] == commands + "512"
    fails(lambda: options(dict(product, modules=["call"]), False), "selected units register 513, maximum is 512")

for path in sorted((ROOT / "products").glob("*/product.json")):
    product = selection.load_product(path)
    manifest = ROOT / "boards" / product["board"] / "board.json"
    board = json.loads(manifest.read_text(encoding="utf-8"))
    options = [line for line in selection.plan(ROOT, product)["options"] if line.startswith((settings, commands))]
    if board.get("memory", {}).get("external_ram") is not False:
        assert not options, (product["name"], options)
    elif product["board"] == "xmini/c3-v3":
        assert options == [settings + "10", commands + "29"], options

# Verify the generated product glue with compiled calls, not just snippets.
with tempfile.TemporaryDirectory(prefix="xiaotai-selection-glue-") as temp:
    root = Path(temp)
    sdk = ROOT / "sdk"
    for name in selection.BASE_COMPONENTS:
        (root / "sdk" / name).mkdir(parents=True)
    write(root, "sdk/modules/control/module.json", {
        "name": "control",
        "configure": [
            {"function": "register_control", "header": "control.h", "section": "register", "form": "call"},
            {"function": "configure_control", "header": "control.h", "type": "control_t",
             "arrays": {"ports": {"type": "unsigned", "count": "port_count"},
                        "labels": {"type": "const char *", "terminated": True}},
             "pointers": {"details": "details_t"}, "overrides": {"mode": "discovery_url"}},
            {"function": "configure_steps", "header": "control.h", "section": "steps", "form": "table",
             "type": "step_t", "pairs": ["name", "delay"], "always": True},
            {"function": "configure_defaults", "header": "control.h", "section": "defaults",
             "type": "control_t", "always": True},
        ]})
    path = root / "sdk/modules/control/Kconfig"
    path.write_text("config XIAOTAI_DIAG\n    bool \"Diagnostics\"\n", encoding="utf-8")
    write(root, "sdk/modules/extra/module.json", {"name": "extra", "configure": [
        {"function": "configure_extra", "header": "control.h", "type": "control_t"}]})
    write(root, "apps/panel/application.json", {"name": "panel", "modules": ["control"],
                                              "optional_modules": ["extra"]})
    name = "products/demo/product.json"
    board_path = "boards/acme/one/board.json"
    write(root, board_path, {"schema": 3, "parts": {}, "identity": {"model": "q\"\\x", "interval": 19}})
    product = {"schema": 1, "name": "demo", "board": "acme/one", "applications": ["panel"],
               "modules": ["extra"], "options": {"DIAG": False},
               "parameters": {
                   "control": {"model": "@board.identity.model", "ports": [7, 9], "labels": ["one", "two"],
                               "details": {"interval": "@board.identity.interval"}, "mode": "normal"},
                   "steps": {"first": 11, "second": 17}, "extra": {"mode": "extra"},
               },
               "variants": {"diag": {"without": ["extra"], "options": {"DIAG": True},
                                     "parameters": {"control": {"mode": "diagnostic"}}}}}
    write(root, name, product)
    loaded = selection.load_product(root / name)
    variant = selection.apply_variant(loaded, "diag")
    assert variant["board"] == loaded["board"] and variant["applications"] == ["panel"]
    assert variant["modules"] == [] and "extra" not in variant["parameters"]
    assert variant["parameters"]["control"]["model"] == "@board.identity.model"
    assert loaded["parameters"]["control"]["mode"] == "normal" and loaded["modules"] == ["extra"]
    fails(lambda: selection.apply_variant(loaded, "typo"), "has no variant typo")
    write(root, name, dict(product, variants={"bad": {"without": ["control"]}}))
    fails(lambda: selection.load_product(root / name), "which the product's \"modules\" do not list")
    baseline = selection.write(root, loaded, root / "normal")
    chosen = selection.write(root, variant, root / "diag")
    assert baseline["modules"] == ["control", "extra"] and chosen["modules"] == ["control"]
    minimum_registries = ["CONFIG_XIAOTAI_FRAMEWORK_MAX_INTENTS=16", "CONFIG_XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS=16"]
    assert baseline["options"] == ["CONFIG_XIAOTAI_DIAG=n", *minimum_registries]
    assert chosen["options"] == ["CONFIG_XIAOTAI_DIAG=y", *minimum_registries]
    assert (root / "diag/sdkconfig.selection").read_text().splitlines() == chosen["options"]
    first = root / "sdk/modules/control/sdkconfig.defaults"
    later = root / "sdk/modules/extra/sdkconfig.defaults"
    first.write_text("CONFIG_DEVICE_FREQ=20\n# CONFIG_PROBE is not set\n", encoding="utf-8")
    later.write_text("CONFIG_OPTIONAL_PATH=y\n", encoding="utf-8")
    assert selection.plan(root, loaded)["sdkconfig"] == [
        "CONFIG_DEVICE_FREQ=20", "CONFIG_PROBE=n", "CONFIG_OPTIONAL_PATH=y"]
    selection.write(root, dict(variant, sdkconfig=["CONFIG_DEVICE_FREQ=40"]), root / "diag")
    assert (root / "diag/sdkconfig.selection").read_text().splitlines() == (
        chosen["options"] + ["CONFIG_PROBE=n", "CONFIG_DEVICE_FREQ=40"])
    later.write_text("CONFIG_DEVICE_FREQ=21\n", encoding="utf-8")
    fails(lambda: selection.plan(root, loaded), "conflicting selected unit defaults for CONFIG_DEVICE_FREQ")
    later.write_text("CONFIG_XIAOTAI_DIAG=y\n", encoding="utf-8")
    fails(lambda: selection.plan(root, loaded), "expected a platform sdkconfig default")
    selection.write(root, variant, root / "diag")

    def with_params(**data):
        return dict(variant, parameters={**variant["parameters"], **data})

    def generate_bad(**data):
        selection.write(root, with_params(**data), root / "reject")

    fails(lambda: generate_bad(control={"model": "@board.identity.absent"}), "no board.identity.absent")
    fails(lambda: generate_bad(control={"model": "@board.identity"}), "not a single value")
    fails(lambda: generate_bad(register={}), "takes no values")
    fails(lambda: generate_bad(steps="invalid"), "takes a list")
    fails(lambda: generate_bad(control=[]), "takes an object")
    fails(lambda: generate_bad(unused={}), "nothing the product selects takes them")

    (root / "control.h").write_text("""
#pragma once
#include "xiaotai/framework.h"
typedef struct { unsigned interval; } details_t;
typedef struct {
    const char *model, *mode;
    const unsigned *ports;
    size_t port_count;
    const char *const *labels;
    const details_t *details;
} control_t;
typedef struct { const char *name; unsigned delay; } step_t;
xiaotai_err_t register_control(void);
xiaotai_err_t configure_control(const control_t *);
xiaotai_err_t configure_steps(const step_t *, size_t);
xiaotai_err_t configure_defaults(const control_t *);
""", encoding="utf-8")
    (root / "main.c").write_text(r"""
#include <assert.h>
#include <string.h>
#include "control.h"
#include "xiaotai/xiaotai.h"
extern xiaotai_err_t xiaotai_selected_configure(const xiaotai_config_t *);
static unsigned stage;
static bool refuse;
static const char *mode = "diagnostic";
xiaotai_err_t xiaotai_module_control_init(void) { return XIAOTAI_OK; }
xiaotai_err_t xiaotai_app_panel_init(void) { return XIAOTAI_OK; }
xiaotai_err_t register_control(void) { assert(stage++ == 0); return XIAOTAI_OK; }
xiaotai_err_t configure_control(const control_t *c) {
    assert(stage++ == 1 && !strcmp(c->model, "q\"\\x") && !strcmp(c->mode, mode));
    assert(c->port_count == 2 && c->ports[0] == 7 && c->ports[1] == 9);
    assert(!strcmp(c->labels[0], "one") && !strcmp(c->labels[1], "two") && c->labels[2] == NULL);
    assert(c->details->interval == 19);
    return refuse ? XIAOTAI_ERR_BUSY : XIAOTAI_OK;
}
xiaotai_err_t configure_steps(const step_t *steps, size_t count) {
    assert(stage++ == 2 && count == 2);
    assert(!strcmp(steps[0].name, "first") && steps[0].delay == 11);
    assert(!strcmp(steps[1].name, "second") && steps[1].delay == 17);
    return XIAOTAI_OK;
}
xiaotai_err_t configure_defaults(const control_t *c) {
    assert(stage++ == 3 && c == NULL);
    return XIAOTAI_OK;
}
int main(void) {
    assert(xiaotai_selected_configure(&(xiaotai_config_t){0}) == XIAOTAI_OK && stage == 4);
    stage = 0;
    mode = "runtime";
    assert(xiaotai_selected_configure(&(xiaotai_config_t){.discovery_url = "runtime"}) == XIAOTAI_OK && stage == 4);
    stage = 0;
    refuse = true;
    assert(xiaotai_selected_configure(&(xiaotai_config_t){.discovery_url = "runtime"}) == XIAOTAI_ERR_BUSY && stage == 2);
    return 0;
}
""", encoding="utf-8")
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror"]
    for include in (root, sdk / "xiaotai_port/host/include",
                    *(sdk / name / "include" for name in ("xiaotai_sdk", "xiaotai_framework", "xiaotai_port"))):
        command += ["-I", str(include)]
    binary = root / "configure-test"
    subprocess.run([*command, str(root / "diag/xiaotai_selected.c"), str(root / "main.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)

worker_defaults = {"KV_WORKER_STACK_BYTES": 3072, "NET_WORKER_STACK_BYTES": 3584,
                   "PLATFORM_SIGNALING_EVENT_STACK_BYTES": 3072}
for path in sorted(ROOT.glob("products/*/product.json")):
    product = selection.load_product(path)
    board = json.loads((ROOT / "boards" / product["board"] / "board.json").read_text(encoding="utf-8"))
    options = dict(line.removeprefix("CONFIG_XIAOTAI_").split("=", 1)
                   for line in selection.plan(ROOT, product, board)["options"])
    demand = "PORT_MQTT_PAYLOAD_ON_DEMAND"
    external = board["memory"]["external_ram"]
    assert options.get(demand) == (None if external else "y"), product["name"]
    dns = "PORT_HTTP_DNS_ON_DEMAND"
    assert options.get(dns) == (None if external else "y"), product["name"]
    changed = {**product, "options": {**product["options"], demand: external}}
    value = "y" if external else "n"
    assert f"CONFIG_XIAOTAI_{demand}={value}" in selection.plan(ROOT, changed, board)["options"]
    changed = {**product, "options": {**product["options"], dns: external}}
    assert f"CONFIG_XIAOTAI_{dns}={value}" in selection.plan(ROOT, changed, board)["options"]
    overlay = {**product, "options": {**product["options"], "PORT_MQTT_NATIVE_OVERLAY": True}}
    assert all(demand not in line for line in selection.plan(ROOT, overlay, board)["options"])
    measured_profile = product["name"] == "xiaotai-xmini-c3-v3"
    assert {k: int(options[k]) for k in worker_defaults if k in options} == (
        worker_defaults if measured_profile else {}), product["name"]
    retained = product["name"] == "xiaotai-xmini-c3-v3"
    assert options.get("AUDIO_RETAIN_I2S_DMA") == ("y" if retained else None), product["name"]
    if retained:
        timed = {**product, "options": {**product["options"], "AUDIO_RETAIN_I2S_DMA": False}}
        assert "CONFIG_XIAOTAI_AUDIO_RETAIN_I2S_DMA=n" in selection.plan(ROOT, timed, board)["options"]
    compact_subscribers = board.get("memory", {}).get("external_ram") is False
    assert options.get("FRAMEWORK_COMPACT_SUBSCRIBERS") == ("y" if compact_subscribers else None), product["name"]
    if compact_subscribers:
        regular = {**product, "options": {**product.get("options", {}), "FRAMEWORK_COMPACT_SUBSCRIBERS": False}}
        assert "CONFIG_XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS=n" in selection.plan(ROOT, regular, board)["options"]
    if measured_profile:
        overrides = {key: 4096 for key in worker_defaults}
        tuned = {**product, "options": {**product.get("options", {}), **overrides}}
        lines = selection.plan(ROOT, tuned, board)["options"]
        assert all(f"CONFIG_XIAOTAI_{key}=4096" in lines for key in worker_defaults)

# The future no-PSRAM coprocessor-board case keeps the network task's native
# Kconfig default; it runs firmware storage and update, not only Wi-Fi.
coprocessor = {"memory": {"external_ram": False}, "parts": {"network": [{"kind": "wifi_coprocessor"}]}}
components = {name: f"sdk/{name}" for name in ("xiaotai_net", "xiaotai_platform", "xiaotai_port")}
lines = selection._options(ROOT, {"name": "fixture"}, components, board=coprocessor)
assert "CONFIG_XIAOTAI_KV_WORKER_STACK_BYTES=3072" in lines
assert "CONFIG_XIAOTAI_PLATFORM_SIGNALING_EVENT_STACK_BYTES=3072" in lines
assert not any("NET_WORKER_STACK_BYTES" in line for line in lines)

table = json.loads((ROOT / "sdk/xiaotai_hal/needs.json").read_text(encoding="utf-8"))
owner_on_main = "FRAMEWORK_OWNER_ON_MAIN"
owner_option = f"CONFIG_XIAOTAI_{owner_on_main}="
memory_profiles = set()
for manifest in sorted((ROOT / "products").glob("*/product.json")):
    product = selection.load_product(manifest)
    external = json.loads((ROOT / "boards" / product["board"] / "board.json").read_text(encoding="utf-8"))["memory"]["external_ram"]
    memory_profiles.add(external)
    defaults = selection.plan(ROOT, product)["options"]
    # External-memory products keep the existing Kconfig default (off).
    assert [line for line in defaults if line.startswith(owner_option)] == ([] if external else [owner_option + "y"])
    override = dict(product, options={**product.get("options", {}), owner_on_main: external})
    assert owner_option + ("y" if external else "n") in selection.plan(ROOT, override)["options"]
    invalid = dict(product, options={**product.get("options", {}), owner_on_main: 1})
    fails(lambda: selection.plan(ROOT, invalid), "expected a boolean")
# Only the single-key C3 product runs without external RAM.
assert memory_profiles == ({False, True} if product_scope.has_product(product_scope.XMINI) else {True})

# The folded media policy is selected by board capacity, not product name.
compact_capture_key = "task.capture.compact_stack"
compact_capture_entry = f'.name = "{compact_capture_key}", .value = '


def media_glue(product, board):
    apps, modules, board = selection.plan(ROOT, product, board)["units"]
    return selection.generate(apps, modules, product, board)


def with_media_parameters(product, values):
    return {**product, "parameters": {**product["parameters"], "media": values}}


if product_scope.has_product(product_scope.XMINI):
    xmini_terminal = selection.load_product(ROOT / "products/xiaotai-xmini-c3-v3/product.json")
    terminal_board = json.loads((ROOT / "boards" / xmini_terminal["board"] / "board.json").read_text(encoding="utf-8"))
    terminal_setting = "    .reserve_owner_terminal = true,\n"
    assert media_glue(xmini_terminal, terminal_board).count(terminal_setting) == 1


    def terminal_glue(values):
        return media_glue(dict(xmini_terminal, parameters={**xmini_terminal["parameters"], "framework": values}),
                          terminal_board)


    assert terminal_setting not in terminal_glue({})
    assert terminal_setting not in terminal_glue({"reserve_owner_terminal": False})
    for invalid in (0, 1, "true", None):
        fails(lambda: terminal_glue({"reserve_owner_terminal": invalid}), "reserve_owner_terminal: expected a boolean")
    fails(lambda: terminal_glue({"queue_depth": 99}), "unknown fields ['queue_depth']")
    fails(lambda: terminal_glue(True), "parameters.framework: expected an object")


for manifest in sorted((ROOT / "products").glob("*/product.json")):
    product = selection.load_product(manifest)
    board = json.loads((ROOT / "boards" / product["board"] / "board.json").read_text(encoding="utf-8"))
    values = product["parameters"]["media"]
    default = media_glue(product, board)
    if board["memory"]["external_ram"]:
        assert compact_capture_entry not in default, product["name"]
        continue
    assert default.count(compact_capture_entry + "1") == 1, product["name"]
    assert compact_capture_key not in values, "generation changed the input manifest"
    unspecified = {**board, "memory": {k: v for k, v in board["memory"].items() if k != "external_ram"}}
    media_only = product
    if product.get("options", {}).get(selection.TIRTC_IDLE_STACK_COMPACT):
        # The separate vendor stack optimization requires a declared capacity.
        # Keep that rejection, then isolate the media default under test.
        fails(lambda: media_glue(product, unspecified), "requires ESP32-C3 without external RAM")
        vendor_options = {k: v for k, v in product["options"].items() if k != selection.TIRTC_IDLE_STACK_COMPACT}
        media_only = dict(product, options=vendor_options)
    assert compact_capture_entry not in media_glue(media_only, unspecified)

    value_list = [{"name": name, "value": value} for name, value in values.items()]
    assert media_glue(with_media_parameters(product, value_list), board) == default
    override = media_glue(with_media_parameters(product, {**values, compact_capture_key: 0}), board)
    assert override == default.replace(compact_capture_entry + "1", compact_capture_entry + "0")
    override_list = value_list + [{"name": compact_capture_key, "value": 0}]
    assert media_glue(with_media_parameters(product, override_list), board) == override
    fails(lambda: media_glue(with_media_parameters(product, override_list + [override_list[-1]]), board),
          compact_capture_key)
    missing_value = value_list + [{"name": compact_capture_key}]
    fails(lambda: media_glue(with_media_parameters(product, missing_value), board), compact_capture_key)
    for invalid in (-1, 2, False, True, 1.0, "1"):
        fails(lambda: media_glue(with_media_parameters(product, {**values, compact_capture_key: invalid}), board),
              compact_capture_key)
        invalid_list = value_list + [{"name": compact_capture_key, "value": invalid}]
        fails(lambda: media_glue(with_media_parameters(product, invalid_list), board), compact_capture_key)

assert selection.NEEDS == {entry["name"]: 1 << entry["bit"] for entry in table["needs"]}
assert selection.BASE_ENGINES == ("eventlog", "at", "identity")
print("PASS: selection order, base engines, optional modules, needs from needs.json, activities of modules "
      "and applications, handles, claims, product preemptions and declared needs, role families (real "
      "door_lock rule, the real lock module selects only with a door relay), board spellings, audio encoding "
      "spellings are the codec vocabulary's audio values and only pcm16 derives pcm_in, network drivers, theme "
      "parameters and pixel spellings, header "
      "activities, absent_with, AT vocabulary (bare AT, option agreement, the applications' commands), "
      "optional_depends/one_of, variants, generated registry sizes/limits, board references, "
      "configure forms and failures, generated tables run, compact capture matrix")
