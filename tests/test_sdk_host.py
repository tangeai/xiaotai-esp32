"""Host build of the SDK: the layer rules, every component's host tests and the
all-headers check (tests/sdk/run.py), and the build helpers themselves
(sdk/cmake/xiaotai_host.cmake) against a small synthetic component tree."""
# Replaces legacy: tests/test_hardware_boundaries.py
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN = ROOT / "tests/sdk/run.py"
sys.path.insert(0, str(ROOT / "scripts"))
import check_sdk_layers  # noqa: E402


def write_tree(root, files):
    for relative, text in files.items():
        path = Path(root) / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")


def run(*arguments, env=None):
    result = subprocess.run([sys.executable, str(RUN), *map(str, arguments)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", timeout=900,
                            env={**os.environ, **(env or {})})
    return result.returncode, result.stdout


def said(output, text):
    """True when the output contains the text; CMake wraps long messages at a column that moves with the path."""
    return " ".join(text.split()) in " ".join(output.split())


def passed_tests(output):
    line = [line for line in output.splitlines() if line.startswith("PASS: SDK host build")][-1]
    return set(line.split(": ", 2)[2].split())


# 1. Layer rules: the real tree is clean, and the rule flags exactly the platform
#    includes outside esp-idf/, host/ and test/ directories, under sdk/ and apps/;
#    a board package has no platform directory at all, only its tests are exempt.
problems = check_sdk_layers.problems(ROOT)
assert not problems, "\n".join(problems)
with tempfile.TemporaryDirectory(prefix="xiaotai-sdk-layers-") as temp:
    write_tree(temp, {
        "sdk/xiaotai_x/src/bad.c": '#include "freertos/FreeRTOS.h"\n',
        "sdk/xiaotai_x/src/good.c": '#include "xiaotai/hal.h"\n#include <string.h>\n',
        "sdk/xiaotai_x/esp-idf/port.c": '#include "esp_log.h"\n#include "driver/gpio.h"\n',
        "sdk/modules/xiaotai_m/host/port.c": "#include <pthread.h>\n",
        "sdk/modules/xiaotai_m/include/xiaotai/m.h": '#include "sdkconfig.h"\n',
        "sdk/parts/xiaotai_p/esp-idf/include/p_port.h": '#include "driver/i2c_master.h"\n',
        "apps/xiaotai_app/test/app_test.c": "#include <unistd.h>\n",
        "apps/xiaotai_app/src/app.c": '  #  include "driver/gpio.h"\n',
        "boards/acme/one/board.cc": '#include "xiaotai/board.hpp"\n#include "driver/gpio.h"\n',
        "boards/acme/one/esp-idf/pins.c": '#include "soc/gpio_num.h"\n',
        "boards/acme/one/test/board_test.cc": "#include <unistd.h>\n",
        "boards/acme/two/board.cc": '#include "xiaotai/board.hpp"\n#include "xiaotai/parts/bus.h"\n',
    })
    found = check_sdk_layers.problems(temp)
    flagged = sorted(line.split(":")[0] for line in found)
    assert flagged == ["apps/xiaotai_app/src/app.c", "boards/acme/one/board.cc", "boards/acme/one/esp-idf/pins.c",
                       "sdk/modules/xiaotai_m/include/xiaotai/m.h", "sdk/xiaotai_x/src/bad.c"], flagged
    assert all("sdk/parts/" in line for line in found if line.startswith("boards/")), found

# 1b. Direction, declarations, libraries and peripherals: a lower layer never
#     includes or requires a higher one, an engine-to-engine or module-to-module
#     edge must be in the manifest, a library header needs the library in the
#     CMakeLists.txt, and only parts and the port create peripherals.
GUARD_ONLY = "// SPDX-License-Identifier: MIT\n#pragma once\n"
with tempfile.TemporaryDirectory(prefix="xiaotai-sdk-direction-") as temp:
    write_tree(temp, {
        "sdk/xiaotai_hal/include/xiaotai/hal.h": GUARD_ONLY + '#include "xiaotai/framework.h"\n',
        "sdk/xiaotai_framework/include/xiaotai/framework.h": GUARD_ONLY,
        "sdk/xiaotai_alpha/engine.json": '{"name": "alpha", "depends": []}\n',
        "sdk/xiaotai_alpha/include/xiaotai/alpha.h": GUARD_ONLY + '#include "xiaotai/modules/m.h"\n',
        "sdk/xiaotai_alpha/CMakeLists.txt": "xiaotai_host_require(xiaotai_framework xiaotai_beta)\n",
        "sdk/xiaotai_beta/engine.json": '{"name": "beta", "depends": ["alpha"]}\n',
        "sdk/xiaotai_beta/include/xiaotai/beta.h": GUARD_ONLY + '#include "xiaotai/alpha.h"\n',
        "sdk/xiaotai_beta/esp-idf/codec.c": "void f(void) { i2s_new_channel(0, 0, 0); }\n",
        "sdk/xiaotai_beta/src/ui.c": '#include "lvgl.h"\n',
        "sdk/parts/xiaotai_part_x/esp-idf/x.c": "void g(void) { i2c_new_master_bus(0, 0); }\n",
        "sdk/modules/xiaotai_mod_m/module.json": '{"name": "m", "depends": ["beta"]}\n',
        "sdk/modules/xiaotai_mod_m/include/xiaotai/modules/m.h": GUARD_ONLY + '#include "xiaotai/beta.h"\n',
        "sdk/modules/xiaotai_mod_n/module.json": '{"name": "n", "depends": []}\n',
        "sdk/modules/xiaotai_mod_n/src/n.c": '#include "xiaotai/modules/m.h"\n',
    })
    found = sorted(check_sdk_layers.problems(temp))
    expected = [
        "sdk/modules/xiaotai_mod_n/src/n.c: includes xiaotai/modules/m.h",
        "sdk/xiaotai_alpha/CMakeLists.txt: names xiaotai_beta",
        "sdk/xiaotai_alpha/include/xiaotai/alpha.h: includes xiaotai/modules/m.h",
        "sdk/xiaotai_beta/esp-idf/codec.c: calls i2s_new_channel()",
        "sdk/xiaotai_beta/src/ui.c: includes lvgl.h of the library lvgl, which its CMakeLists.txt does not name",
        "sdk/xiaotai_hal/include/xiaotai/hal.h: includes xiaotai/framework.h",
    ]
    assert len(found) == len(expected) and all(line.startswith(prefix) for line, prefix in zip(found, expected)), \
        "\n".join(found)

with tempfile.TemporaryDirectory(prefix="xiaotai-sdk-peripherals-") as temp:
    # Native engine folders can include driver headers, but still cannot own
    # shared hardware. Exercise the same creation calls as the legacy check.
    forbidden = {
        "sdk/xiaotai_media/esp-idf/bus.c": "i2c_new_master_bus",
        "sdk/xiaotai_media/esp-idf/dma.c": "i2s_new_channel",
        "sdk/xiaotai_media/esp-idf/codec.c": "esp_codec_dev_new",
        "apps/xiaotai_console/esp-idf/lcd.c": "esp_lcd_new_panel_st7789",
        "sdk/xiaotai_ui/esp-idf/spi.c": "spi_bus_initialize",
    }
    allowed = {
        "sdk/parts/xiaotai_part_bus/esp-idf/bus.c": "i2c_new_master_bus",
        "sdk/parts/xiaotai_part_audio/esp-idf/codec.c": "esp_codec_dev_new",
        "sdk/xiaotai_port/esp-idf/spi.c": "spi_bus_initialize",
    }
    write_tree(temp, {p: f"void f(void) {{ {name}(0); }}\n" for p, name in (forbidden | allowed).items()})
    found = check_sdk_layers.problems(temp)
    expected = [f"{p}: calls {name}();" for p, name in forbidden.items()]
    assert len(found) == len(expected) and all(any(line.startswith(prefix) for line in found)
                                              for prefix in expected), "\n".join(found)

# 1c. Portable code: no platform configuration or target macro outside a
#     configuration header, no direct RTOS or ESP-IDF call, no board, chip or
#     product name (comments included, documented examples excepted); an
#     intent whose handler forgets credentials must be local-only; published
#     event, intent and setting names start with the owner's manifest name or
#     a domain registered to it.
FORGETTING = (
    "static void wipe(void) { (void)xiaotai_identity_erase(0, 0); }\n"
    "static void on_reset(const xiaotai_args_t *a, void *c) { (void)a; (void)c; if (1) { wipe(); } }\n"
    "static void on_forget(const xiaotai_args_t *a, void *c) { (void)a; (void)c; "
    "(void)xiaotai_provisioning_begin(XIAOTAI_PROVISIONING_FORGET); }\n"
    "static void on_retry(const xiaotai_args_t *a, void *c) { (void)a; (void)c; xiaotai_net_reprovision(false); }\n")
with tempfile.TemporaryDirectory(prefix="xiaotai-sdk-portable-") as temp:
    write_tree(temp, {
        "sdk/xiaotai_x/src/target.c": "#if CONFIG_IDF_TARGET_ESP32P4\n#endif\n",
        "sdk/xiaotai_x/src/rtos.c": "void f(void) { vTaskDelay(1); }\n",
        "sdk/xiaotai_x/src/named.c": "/* Tuned on the Xmini board. */\n",
        "sdk/xiaotai_x/src/chip.c": 'static const char *chip = "esp32c3";\n',
        "sdk/xiaotai_x/src/word.c": "/* Same on S3 and P4. */\n",
        "sdk/xiaotai_x/src/fine.c": ("/* A comment about CONFIG_FOO is only prose; platform words like s3cret "
                                     "or p40 are not names. */\nstatic const char *t = \"esp_x\";\n"),
        "sdk/xiaotai_x/include/xiaotai/x_config.h": "#ifdef CONFIG_XIAOTAI_X_DEPTH\n#endif\n",
        "sdk/xiaotai_x/esp-idf/x.c": "#if CONFIG_IDF_TARGET_ESP32S3\nvoid g(void) { vTaskDelay(1); }\n#endif\n",
        "sdk/xiaotai_x/test/x_test.c": "/* esp32p4 */\n",
        "sdk/modules/xiaotai_mod_q/module.json": '{"name": "q"}\n',
        "sdk/modules/xiaotai_mod_q/include/xiaotai/modules/q.h": (
            '#define XIAOTAI_EVENT_Q_OK "q.ok"\n#define XIAOTAI_INTENT_Q_TALK "talk.go"\n'
            '#define XIAOTAI_SETTING_Q_OTHER "zzz.no"\n#define XIAOTAI_INTENT_Q_SYSTEM "system.q"\n'),
        "apps/xiaotai_app_y/src/reset.c": FORGETTING + (
            "void reg(void)\n{\n"
            '    (void)xiaotai_intent_handle("y.reset", on_reset, 0);\n'
            '    if (xiaotai_intent_handle_ex("y.forget", on_forget, 0, 0) != 0) return;\n'
            '    (void)xiaotai_intent_handle_ex("y.local", on_forget, 0, XIAOTAI_INTENT_LOCAL);\n'
            '    (void)xiaotai_intent_handle("y.retry", on_retry, 0);\n'
            "}\n"),
    })
    found = sorted(check_sdk_layers.problems(temp))
    expected = [
        "apps/xiaotai_app_y/src/reset.c:7: intent handler on_reset",
        "apps/xiaotai_app_y/src/reset.c:8: intent handler on_forget",
        'sdk/modules/xiaotai_mod_q/include/xiaotai/modules/q.h:2: publishes "talk.go"',
        'sdk/modules/xiaotai_mod_q/include/xiaotai/modules/q.h:3: publishes "zzz.no"',
        'sdk/modules/xiaotai_mod_q/include/xiaotai/modules/q.h:4: publishes "system.q"',
        "sdk/xiaotai_x/src/chip.c:1: names esp32c3",
        "sdk/xiaotai_x/src/named.c:1: names Xmini",
        "sdk/xiaotai_x/src/rtos.c:1: uses vTaskDelay",
        "sdk/xiaotai_x/src/target.c:1: reads CONFIG_IDF_TARGET_ESP32P4",
        "sdk/xiaotai_x/src/word.c:1: names P4",
        "sdk/xiaotai_x/src/word.c:1: names S3",
    ]
    assert len(found) == len(expected) and all(line.startswith(prefix) for line, prefix in zip(found, expected)), \
        "\n".join(found)

# 1d. The need table: a vocabulary value must be defined (enumerator or macro)
#     by hal.h or a header it includes, not merely named in a comment; the
#     kind and role tables of src/hal.cc match the needs.json rules row by row,
#     and a role rule on a class hal.cc does not derive roles for is refused.
NEED_TABLE = {
    "schema": 1,
    "needs": [{"name": "camera", "bit": 0, "part": "camera", "where": {"role": ["front"]}},
              {"name": "key", "bit": 1, "part": "input", "where": {"kind": ["button"]}},
              {"name": "relay", "bit": 2, "part": "output", "where": {"kind": ["relay"]}},
              {"name": "door_lock", "bit": 3, "part": "output", "where": {"kind": ["relay"], "role": ["door"]}}],
    "vocabulary": {"input.kind": {"button": "XIAOTAI_INPUT_BUTTON"},
                   "output.kind": {"relay": "XIAOTAI_OUTPUT_RELAY", "light": "XIAOTAI_OUTPUT_LIGHT"},
                   "audio_input.encoding": {"pcm16": "XIAOTAI_CODEC_PCM16", "opus": "XIAOTAI_CODEC_OPUS"}},
}
with tempfile.TemporaryDirectory(prefix="xiaotai-sdk-needs-") as temp:
    write_tree(temp, {
        "sdk/xiaotai_hal/needs.json": json.dumps(NEED_TABLE),
        "sdk/xiaotai_port/include/xiaotai/needs.h": GUARD_ONLY + "enum { XIAOTAI_NEED_CAMERA = 1u << 0, "
        "XIAOTAI_NEED_KEY = 1u << 1, XIAOTAI_NEED_RELAY = 1u << 2, XIAOTAI_NEED_DOOR_LOCK = 1u << 3 };\n",
        "sdk/xiaotai_port/include/xiaotai/codec.h": GUARD_ONLY + "typedef enum { XIAOTAI_CODEC_NONE = 0, "
        "XIAOTAI_CODEC_PCM16 = 5 } xiaotai_codec_t;\n",
        "sdk/xiaotai_hal/include/xiaotai/hal.h": GUARD_ONLY + '#include "xiaotai/codec.h"\n'
        "/* Encodings: XIAOTAI_CODEC_PCM16, XIAOTAI_CODEC_OPUS. */\n"
        "enum { XIAOTAI_INPUT_BUTTON = 1 };\nenum {\n    XIAOTAI_OUTPUT_LIGHT = 1,\n    XIAOTAI_OUTPUT_RELAY,\n};\n",
        "sdk/xiaotai_hal/src/hal.cc": (
            "constexpr KindNeed kInputNeeds[] = {\n    {XIAOTAI_INPUT_BUTTON, XIAOTAI_NEED_KEY},\n};\n"
            "constexpr KindNeed kOutputNeeds[] = {\n    {XIAOTAI_OUTPUT_RELAY, XIAOTAI_NEED_RELAY},\n"
            "    {XIAOTAI_OUTPUT_LIGHT, XIAOTAI_NEED_RELAY},\n};\n"
            "// {XIAOTAI_OUTPUT_RELAY, \"gate\", XIAOTAI_NEED_DOOR_LOCK} is only a comment\n"
            "constexpr RoleNeed kOutputRoleNeeds[] = {\n    {XIAOTAI_OUTPUT_RELAY, \"door\", XIAOTAI_NEED_DOOR_LOCK},\n};\n"),
    })
    found = sorted(line for line in check_sdk_layers.problems(temp) if line.startswith("sdk/xiaotai_hal/"))
    expected = [
        "sdk/xiaotai_hal/needs.json: XIAOTAI_CODEC_OPUS is not defined",
        "sdk/xiaotai_hal/needs.json: need camera has a role rule on part class camera",
        "sdk/xiaotai_hal/src/hal.cc: kOutputNeeds row {XIAOTAI_OUTPUT_LIGHT, XIAOTAI_NEED_RELAY} has no needs.json",
    ]
    assert len(found) == len(expected) and all(line.startswith(prefix) for line, prefix in zip(found, expected)), \
        "\n".join(found)
    NEED_TABLE["needs"][3]["where"]["role"] = ["gate"]
    write_tree(temp, {"sdk/xiaotai_hal/needs.json": json.dumps(NEED_TABLE)})
    found = [line for line in check_sdk_layers.problems(temp) if "kOutputRoleNeeds" in line]
    assert len(found) == 2 and "lacks {XIAOTAI_OUTPUT_RELAY, gate, XIAOTAI_NEED_DOOR_LOCK}" in found[0] \
        and "row {XIAOTAI_OUTPUT_RELAY, door, XIAOTAI_NEED_DOOR_LOCK} has no needs.json rule" in found[1], found

# 2. The real tree: every component found, its tests, and the all-headers check.
subprocess.run([sys.executable, str(ROOT / "tests/sdk/test_host_roles.py")], check=True, timeout=480)
code, output = run()
assert code == 0, output
real = passed_tests(output)
assert {"sdk_all_headers", "port_conformance", "port_kv", "framework_core", "hal_board", "media_downlink",
        "sdk_public_api", "sdk_start"} <= real, real

# 3. The helpers, on a synthetic tree that uses every search root: a dependency
#    chain pulled in on demand (application -> engine -> module -> theme), a
#    component that is only headers and has no CMakeLists.txt, platform
#    directories, a test that fakes its dependency through INCLUDE_ONLY, and two
#    board packages that share the component name xiaotai_board and a test name.
GUARD = "// SPDX-License-Identifier: MIT\n#pragma once\n"
C_OPEN = '#ifdef __cplusplus\nextern "C" {\n#endif\n'
C_CLOSE = "#ifdef __cplusplus\n}\n#endif\n"
TESTS = "if(XIAOTAI_HOST_TESTS)\n    add_subdirectory(test)\nendif()\n"
FIXTURE = {
    "sdk/xiaotai_alpha/CMakeLists.txt": (
        "set(sources src/alpha.c)\n"
        "if(ESP_PLATFORM)\n"
        '    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES xiaotai_beta xiaotai_gamma)\n'
        "    return()\n"
        "endif()\n"
        "xiaotai_host_require(xiaotai_beta xiaotai_gamma)\n"
        "add_library(xiaotai_alpha STATIC ${sources})\n"
        "target_include_directories(xiaotai_alpha PUBLIC include)\n"
        "target_link_libraries(xiaotai_alpha PUBLIC xiaotai_beta xiaotai_gamma)\n" + TESTS),
    "sdk/xiaotai_alpha/include/xiaotai/alpha.h": (
        GUARD + '#include "xiaotai/beta.h"\n' + C_OPEN + "int xiaotai_alpha_value(void);\n" + C_CLOSE),
    "sdk/xiaotai_alpha/src/alpha.c": (
        '#include "xiaotai/alpha.h"\n#include "xiaotai/gamma.h"\n'
        "int xiaotai_alpha_value(void)\n{\n    return XIAOTAI_BETA_BASE + xiaotai_gamma_value();\n}\n"),
    "sdk/xiaotai_alpha/test/CMakeLists.txt": "xiaotai_host_test(alpha_real SOURCES alpha_test.c LINK xiaotai_alpha)\n",
    "sdk/xiaotai_alpha/test/alpha_test.c": (
        '#include <limits.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include "xiaotai/alpha.h"\n'
        "int main(void)\n{\n"
        '    if (getenv("XIAOTAI_FIXTURE_FAIL")) {\n        fprintf(stderr, "fixture failure marker\\n");\n'
        "        return 1;\n    }\n"
        '    if (getenv("XIAOTAI_FIXTURE_UBSAN")) {\n'
        "        volatile unsigned shift = sizeof(unsigned) * CHAR_BIT;\n"
        "        volatile unsigned bad = 1U << shift;\n"
        "        (void)bad;\n"
        "    }\n"
        "    return xiaotai_alpha_value() == 42 ? 0 : 1;\n}\n"),
    "sdk/parts/xiaotai_beta/include/xiaotai/beta.h": GUARD + "#define XIAOTAI_BETA_BASE 40\n",
    "sdk/parts/xiaotai_beta/include/xiaotai/beta.hpp": (
        GUARD + '#include "xiaotai/beta.h"\n'
        "namespace xiaotai {\ninline int BetaBase() { return XIAOTAI_BETA_BASE; }\n}  // namespace xiaotai\n"),
    "sdk/modules/xiaotai_gamma/CMakeLists.txt": (
        "xiaotai_host_require(xiaotai_delta)\n"
        "add_library(xiaotai_gamma STATIC src/gamma.c)\n"
        "target_include_directories(xiaotai_gamma PUBLIC include)\n"
        "target_link_libraries(xiaotai_gamma PUBLIC xiaotai_delta)\n" + TESTS),
    "sdk/modules/xiaotai_gamma/include/xiaotai/gamma.h": (
        GUARD + C_OPEN + "int xiaotai_gamma_value(void);\n" + C_CLOSE),
    "sdk/modules/xiaotai_gamma/src/gamma.c": (
        '#include "xiaotai/gamma.h"\n#include "xiaotai/delta.h"\n'
        "int xiaotai_gamma_value(void)\n{\n    return xiaotai_delta_value() + 1;\n}\n"),
    # The second test links a component that itself depends on this one: it is
    # required on demand when gamma is built alone, and resolved late when alpha
    # is the one being configured.
    "sdk/modules/xiaotai_gamma/test/CMakeLists.txt": (
        "xiaotai_host_test(gamma_real SOURCES gamma_test.c LINK xiaotai_gamma)\n"
        "xiaotai_host_test(gamma_with_alpha SOURCES gamma_alpha_test.c LINK xiaotai_alpha TIMEOUT 30)\n"),
    "sdk/modules/xiaotai_gamma/test/gamma_test.c": (
        '#include "xiaotai/gamma.h"\nint main(void)\n{\n    return xiaotai_gamma_value() == 2 ? 0 : 1;\n}\n'),
    "sdk/modules/xiaotai_gamma/test/gamma_alpha_test.c": (
        '#include "xiaotai/alpha.h"\nint main(void)\n{\n    return xiaotai_alpha_value() == 42 ? 0 : 1;\n}\n'),
    "sdk/themes/xiaotai_delta/CMakeLists.txt": (
        "add_library(xiaotai_delta STATIC host/delta.c)\n"
        "target_include_directories(xiaotai_delta PUBLIC include)\n"),
    "sdk/themes/xiaotai_delta/include/xiaotai/delta.h": (
        GUARD + C_OPEN + "int xiaotai_delta_value(void);\n" + C_CLOSE),
    "sdk/themes/xiaotai_delta/host/delta.c": (
        '#include <unistd.h>\n#include "xiaotai/delta.h"\n'
        "int xiaotai_delta_value(void)\n{\n    return getpid() > 0 ? 1 : 0;\n}\n"),
    "apps/xiaotai_app_demo/CMakeLists.txt": (
        "xiaotai_host_require(xiaotai_alpha)\n"
        "add_library(xiaotai_app_demo STATIC src/demo.c)\n"
        "target_include_directories(xiaotai_app_demo PUBLIC include)\n"
        "target_link_libraries(xiaotai_app_demo PUBLIC xiaotai_alpha)\n" + TESTS),
    "apps/xiaotai_app_demo/include/xiaotai/app_demo.h": (
        GUARD + C_OPEN + "int xiaotai_app_demo_value(void);\n" + C_CLOSE),
    "apps/xiaotai_app_demo/src/demo_internal.h": GUARD + "#define XIAOTAI_APP_DEMO_OFFSET 1\n",
    "apps/xiaotai_app_demo/src/demo.c": (
        '#include "xiaotai/app_demo.h"\n#include "xiaotai/alpha.h"\n#include "demo_internal.h"\n'
        "int xiaotai_app_demo_value(void)\n{\n    return xiaotai_alpha_value() + XIAOTAI_APP_DEMO_OFFSET;\n}\n"),
    # The fake replaces the engine: nothing may be linked, only include paths added
    # (the application's, and those of everything it publicly depends on).
    "apps/xiaotai_app_demo/test/CMakeLists.txt": (
        "xiaotai_host_test(app_demo_fake SOURCES demo_test.c ../src/demo.c\n"
        "                  INCLUDE ../src INCLUDE_ONLY xiaotai_app_demo)\n"
        "get_target_property(linked app_demo_fake LINK_LIBRARIES)\n"
        "if(linked)\n"
        '    message(FATAL_ERROR "INCLUDE_ONLY linked ${linked}")\n'
        "endif()\n"),
    "apps/xiaotai_app_demo/test/demo_test.c": (
        '#include "xiaotai/app_demo.h"\n#include "xiaotai/alpha.h"\n#include "xiaotai/gamma.h"\n'
        '#include "demo_internal.h"\n'
        "int xiaotai_alpha_value(void)\n{\n    return 7;\n}\n"
        "int main(void)\n{\n    return xiaotai_app_demo_value() == 7 + XIAOTAI_APP_DEMO_OFFSET ? 0 : 1;\n}\n"),
}
BOARD_CMAKE = (
    "if(ESP_PLATFORM)\n"
    '    idf_component_register(SRCS "board.c" REQUIRES xiaotai_beta)\n'
    "    return()\n"
    "endif()\n"
    "xiaotai_host_require(xiaotai_beta)\n"
    "add_library(xiaotai_board STATIC board.c)\n"
    "target_link_libraries(xiaotai_board PUBLIC xiaotai_beta)\n" + TESTS)
BOARD_TEST = (
    "int xiaotai_board_value(void);\n"
    "int main(void)\n{\n    return xiaotai_board_value() == EXPECTED ? 0 : 1;\n}\n")
for board, offset in (("acme/one", 1), ("acme/two", 2)):
    FIXTURE.update({
        f"boards/{board}/CMakeLists.txt": BOARD_CMAKE,
        f"boards/{board}/board.c": (
            '#include "xiaotai/beta.h"\n'
            f"int xiaotai_board_value(void)\n{{\n    return XIAOTAI_BETA_BASE + {offset};\n}}\n"),
        f"boards/{board}/test/CMakeLists.txt": (
            "xiaotai_host_test(board_parts SOURCES board_test.c LINK xiaotai_board)\n"
            f"target_compile_definitions(board_parts PRIVATE EXPECTED={40 + offset})\n"),
        f"boards/{board}/test/board_test.c": BOARD_TEST,
    })
BROKEN = {
    # Uses size_t without including what declares it.
    "header": {"sdk/parts/xiaotai_beta/include/xiaotai/needs_other.h": GUARD + "size_t xiaotai_beta_size(void);\n"},
    # Defines an object: two translation units that include it cannot be linked.
    "object": {"sdk/parts/xiaotai_beta/include/xiaotai/defines.h": GUARD + "int xiaotai_beta_counter = 0;\n"},
    "no_host_branch": {"sdk/xiaotai_epsilon/CMakeLists.txt": 'idf_component_register(SRCS "e.c")\n'},
    "test_not_added": {"sdk/themes/xiaotai_delta/test/CMakeLists.txt":
                       "xiaotai_host_test(delta_real SOURCES t.c LINK xiaotai_delta)\n",
                       "sdk/themes/xiaotai_delta/test/t.c": "int main(void)\n{\n    return 0;\n}\n"},
    "cycle": {"sdk/themes/xiaotai_delta/CMakeLists.txt": (
        "xiaotai_host_require(xiaotai_alpha)\n" + FIXTURE["sdk/themes/xiaotai_delta/CMakeLists.txt"])},
    "duplicate_test": {"sdk/modules/xiaotai_gamma/test/CMakeLists.txt": (
        "xiaotai_host_test(alpha_real SOURCES gamma_test.c LINK xiaotai_gamma)\n")},
}
EXPECTED = {
    "header": "needs_other", "object": "xiaotai_beta_counter", "no_host_branch": "idf_component_register() reached",
    "test_not_added": "never adds it", "cycle": "dependency cycle", "duplicate_test": "already taken",
}

with tempfile.TemporaryDirectory(prefix="xiaotai-sdk-fixture-") as temp:
    good = Path(temp) / "good"
    write_tree(good, FIXTURE)
    scratch = Path(temp) / "scratch"
    scratch.mkdir()
    kept_parent_env = {"TMPDIR": str(scratch)}
    for name, extra in BROKEN.items():
        write_tree(Path(temp) / name, {**FIXTURE, **extra})
    # Started together: each run has its own build directory.
    with ThreadPoolExecutor(max_workers=6) as pool:
        everything = pool.submit(run, "--root", good)
        one = pool.submit(run, "--root", good, "--keep", "xiaotai_gamma", env=kept_parent_env)
        own = pool.submit(run, "--root", good, "--skip-dependency-tests", "xiaotai_app_demo")
        failing = pool.submit(run, "--root", good, "xiaotai_alpha",
                              env={**kept_parent_env, "XIAOTAI_FIXTURE_FAIL": "1"})
        undefined = pool.submit(run, "--root", good, "xiaotai_alpha", env={"XIAOTAI_FIXTURE_UBSAN": "1"})
        unknown = pool.submit(run, "--root", good, "xiaotai_missing")
        one_board = pool.submit(run, "--root", good, "--board", "acme/two")
        with_board = pool.submit(run, "--root", good, "xiaotai_gamma", "--board", "acme/one",
                                 "--skip-dependency-tests")
        all_boards = pool.submit(run, "--root", good, "--all-boards")
        no_board = pool.submit(run, "--root", good, "--board", "acme/three")
        board_as_component = pool.submit(run, "--root", good, "xiaotai_board")
        broken = {name: pool.submit(run, "--root", Path(temp) / name) for name in BROKEN}

    code, output = everything.result()
    assert code == 0, output
    board_tests = {f"acme/{name}:{test}" for name in ("one", "two") for test in ("board_parts", "sdk_all_headers")}
    assert passed_tests(output) == {"alpha_real", "gamma_real", "gamma_with_alpha", "app_demo_fake",
                                    "sdk_all_headers"} | board_tests, output
    code, output = one_board.result()      # one board: the board and what it depends on, nothing else
    assert code == 0 and passed_tests(output) == {"acme/two:board_parts", "acme/two:sdk_all_headers"}, output
    code, output = with_board.result()     # components and a board: two builds, one report
    assert code == 0 and passed_tests(output) == {"gamma_real", "gamma_with_alpha", "sdk_all_headers",
                                                  "acme/one:board_parts", "acme/one:sdk_all_headers"}, output
    code, output = all_boards.result()
    assert code == 0 and passed_tests(output) == board_tests, output
    code, output = no_board.result()
    assert code != 0 and said(output, "No board package acme/three"), output
    code, output = board_as_component.result()   # the shared name means nothing without a board
    assert code != 0 and said(output, "needs a board"), output
    code, output = one.result()            # gamma, what it and its tests depend on, and not the application
    assert code == 0 and passed_tests(output) == {"gamma_real", "gamma_with_alpha", "alpha_real",
                                                  "sdk_all_headers"}, output
    keep_prefix = "build directory kept: "
    saved = [Path(line[len(keep_prefix):]) for line in output.splitlines() if line.startswith(keep_prefix)]
    assert len(saved) == 1 and saved[0].parent == scratch.resolve(), output
    assert (saved[0] / "CMakeCache.txt").is_file() and list(scratch.iterdir()) == saved
    code, output = own.result()            # dependencies are built, only the named component is tested
    assert code == 0 and passed_tests(output) == {"app_demo_fake", "sdk_all_headers"}, output
    code, output = failing.result()        # a failing test fails the run and its output is shown
    assert code != 0 and "fixture failure marker" in output and "FAIL: SDK host tests" in output, output
    assert "gamma_real" in output and "PASS: SDK host build" not in output, output
    code, output = undefined.result()
    assert code != 0 and "runtime error: shift exponent" in output and "FAIL: SDK host tests" in output, output
    code, output = unknown.result()
    assert code != 0 and said(output, "No component xiaotai_missing"), output
    for name, future in broken.items():
        code, output = future.result()
        assert code != 0 and EXPECTED[name] in output, (name, output)

print(f"PASS: SDK layer rules, host build and {len(real)} tests of the real components, all public headers in "
      "C and C++, build helpers (on-demand dependencies, header-only components, INCLUDE_ONLY fakes, component "
      "selection, board packages one per build, parallel runs, failure reporting)")
