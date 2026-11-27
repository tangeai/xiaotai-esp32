# SPDX-License-Identifier: MIT
"""Run the hash-pinned native MQTT task with a per-client owner bridge (no network)."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
from tempfile import TemporaryDirectory

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
ROOT = PORT.parents[1]
sys.path.insert(0, str(PORT / "esp-idf"))
from mqtt_owner_overlay import owner_header, owner_source  # noqa: E402
from mqtt_native_overlay import plan, write_candidate  # noqa: E402
from mqtt_native_overlay_test import syntax as native_plan_syntax  # noqa: E402
from mqtt_native_fixture import (write_compat, excerpt, locked_bytes, MSG_SHA256,
                                     OUTBOX_SHA256)  # noqa: E402


FUNCTIONS = (
    "static inline void mqtt_reset_pending_message(",
    "static inline esp_err_t esp_mqtt_write(",
    "static void esp_mqtt_abort_connection(esp_mqtt_client_handle_t client)\n{",
    "static outbox_item_handle_t mqtt_enqueue(",
    "static int make_publish(",
    "static inline int mqtt_client_enqueue_publish(",
    "int esp_mqtt_client_publish(",
    "int esp_mqtt_client_subscribe_multiple(",
    "static esp_err_t send_disconnect_msg(esp_mqtt_client_handle_t client)\n{",
    "static esp_err_t esp_mqtt_dispatch_event(esp_mqtt_client_handle_t client)\n{",
    "static void esp_mqtt_client_dispatch_transport_error(esp_mqtt_client_handle_t client)\n{",
    "static inline void run_event_loop(",
    "static void esp_mqtt_task(",
    "esp_err_t esp_mqtt_client_stop(",
)


def wrapper_syntax(component, database):
    rows = [row for row in json.loads(database.read_text(encoding="utf-8"))
            if Path(row["file"]).name == "mqtt_client.c"]
    if len(rows) != 1:
        raise ValueError("expected one native MQTT command for the real IDF compiler/configuration")
    row = rows[0]
    original = row.get("arguments") or shlex.split(row["command"])
    command = [original[0]]
    for include in (PORT / "include", PORT / "esp-idf/include", PORT / "esp-idf", component / "include"):
        command += ["-I", str(include)]
    skip = False
    for argument in original[1:]:
        if skip:
            skip = False
            continue
        if argument in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
        elif argument not in ("-c", "-MD", "-MMD", row["file"]):
            command.append(argument)
    subprocess.run([*command, "-DCONFIG_XIAOTAI_PORT_MQTT_NATIVE_OVERLAY=1",
                    "-fsyntax-only", str(PORT / "esp-idf/mqtt.c")], cwd=row["directory"],
                   check=True, timeout=30)
    print(f"PASS: actual port wrapper syntax only; compiler={original[0]}")


def run(component, cc, repeat=1, port=False, parser_dir=None):
    raw = (component / "mqtt_client.c").read_bytes()
    generated = plan(component) if port else None
    source = (generated["mqtt_client.c"] if port else owner_source(raw)).decode("utf-8")
    header = generated["lib/include/mqtt_client_priv.h"] if port else owner_header(
        (component / "lib/include/mqtt_client_priv.h").read_bytes())
    with TemporaryDirectory(prefix="xiaotai-mqtt-owner-integration-") as directory:
        scratch = Path(directory)
        write_compat(scratch)
        if port:
            native = scratch / "native"
            for name, data in generated.items():
                path = native / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
        else:
            (scratch / "mqtt_client_priv.h").write_bytes(header)
        functions = FUNCTIONS + (("esp_err_t esp_mqtt_client_start(", "int esp_mqtt_client_enqueue(",
                                  "esp_mqtt_client_connection_state_t esp_mqtt_client_get_state(",
                                  "int esp_mqtt_client_get_outbox_size(", "static esp_err_t mqtt_resend_queued(") if port else ())
        if port:
            functions += ("static void xt_native_mqtt_wipe_string(", "static esp_err_t esp_mqtt_check_cfg_conflict(",
                          "bool esp_mqtt_set_if_config(", "esp_err_t esp_mqtt_set_config(",
                          "void esp_mqtt_destroy_config(", "static bool create_client_data(",
                          "esp_mqtt_client_handle_t esp_mqtt_client_init(", "esp_err_t esp_mqtt_client_destroy(",
                          "esp_err_t esp_mqtt_client_set_uri(")
        prefix = ('#include "esp_heap_caps.h"\n#include "mqtt_utils.h"\n#include "http_parser.h"\n'
                  '#include <ctype.h>\n#include <net/if.h>\n#include "mqtt_queue_client.inc"\n'
                  '#undef ESP_MEM_CHECK\n#define ESP_MEM_CHECK(tag, value, action) '
                  'do { (void)(tag); if (!(value)) { action; } } while (0);\n') if port else ""
        suffix = source[source.index('#line 1 "mqtt_enqueue_connected.inc"'):] if port else ""
        bodies = "\n".join(excerpt(source, start) for start in functions)
        if port:
            bodies = bodies.replace("esp_mqtt_client_handle_t esp_mqtt_client_init(",
                                    "static esp_mqtt_client_handle_t fixture_real_native_init(").replace(
                "esp_err_t esp_mqtt_client_destroy(", "static esp_err_t fixture_real_native_destroy(")
            stock_init = excerpt(raw.decode("utf-8"), "esp_mqtt_client_handle_t esp_mqtt_client_init(").replace(
                "esp_mqtt_client_handle_t esp_mqtt_client_init(",
                "static esp_mqtt_client_handle_t fixture_stock_loop_init(")
            bodies += "\n" + stock_init
        (scratch / "mqtt_owner_native.inc").write_text(prefix + bodies + suffix,
                                                       encoding="utf-8")
        group = scratch / "freertos/event_groups.h"
        group.write_text(group.read_text(encoding="ascii").replace("typedef void *EventGroupHandle_t;",
                          "typedef struct host_group *EventGroupHandle_t;") + """
#include <stdbool.h>
unsigned xEventGroupSetBits(EventGroupHandle_t group, unsigned bits);
unsigned xEventGroupClearBits(EventGroupHandle_t group, unsigned bits);
unsigned xEventGroupGetBits(EventGroupHandle_t group);
unsigned xEventGroupWaitBits(EventGroupHandle_t group, unsigned bits, bool clear, bool all, int ticks);
#define portTICK_PERIOD_MS 1
""", encoding="ascii")
        if port:
            event = scratch / "esp_event.h"
            with event.open("a", encoding="ascii") as output:
                output.write("""
typedef struct { int queue_size; const char *task_name; } esp_event_loop_args_t;
#define esp_event_loop_create fixture_lifecycle_event_loop_create
#define esp_event_loop_delete fixture_lifecycle_event_loop_delete
""")
            group.write_text(group.read_text(encoding="ascii").replace(
                "static inline int xSemaphoreTakeRecursive", "void fixture_before_api_lock(SemaphoreHandle_t lock, int ticks);\n"
                "static inline int xSemaphoreTakeRecursive").replace(
                "const int rc = ticks ? pthread_mutex_lock(lock)",
                "fixture_before_api_lock(lock, ticks);\n                const int rc = ticks ? pthread_mutex_lock(lock)"),
                encoding="ascii")
            with group.open("a", encoding="ascii") as output:
                output.write("""
#include <stdlib.h>
#define xSemaphoreCreateRecursiveMutex fixture_lifecycle_mutex_create
#define vSemaphoreDelete fixture_lifecycle_mutex_delete
#define xEventGroupCreate fixture_lifecycle_group_create
#define vEventGroupDelete fixture_lifecycle_group_delete
#if !XIAOTAI_OWNER_REAL_LIFECYCLE
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
    pthread_mutex_t *lock = malloc(sizeof(*lock));
    pthread_mutexattr_t attr;
    assert(lock && !pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(lock, &attr) && !pthread_mutexattr_destroy(&attr));
    return lock;
}
static inline void vSemaphoreDelete(SemaphoreHandle_t lock) {
    assert(!pthread_mutex_destroy(lock)); free(lock);
}
#endif
""")
            for name in ("FreeRTOS.h", "semphr.h"):
                (scratch / "freertos" / name).write_text('#pragma once\n#include "event_groups.h"\n', encoding="ascii")
        (scratch / "freertos/task.h").write_text("#pragma once\n#include \"freertos/event_groups.h\"\n"
                                                  "TaskHandle_t xTaskGetCurrentTaskHandle(void);\n"
                                                  "#define tskNO_AFFINITY (-1)\n"
                                                  "int xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned, "
                                                  "void *, unsigned, TaskHandle_t *, int);\n", encoding="ascii")
        sources = (component / "lib/mqtt_msg.c", component / "lib/mqtt_outbox.c")
        for path, digest in zip(sources, (MSG_SHA256, OUTBOX_SHA256)):
            locked_bytes(path, digest)
        binary = scratch / "owner"
        command = [cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                   "-Wno-unused-function", "-pthread", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-fno-pie", "-no-pie", "-include", "mqtt_native_compat.h"]
        includes = (scratch, component / "include", component / "lib/include", PORT / "esp-idf", PORT / "include")
        if port:
            if parser_dir is None:
                raise ValueError("the joined native lifecycle proof requires the local HTTP parser source")
            parser_pins = {"http_parser.c": "bc5750d936bdb1ca6064f65ec31f66578c9fee2a691ac298081fbc73a668be34",
                           "http_parser.h": "caa7e5e6571a9067008b1bcb999c46f84157121163df2abec537fc39eeb8ba4d"}
            for name, digest in parser_pins.items():
                locked_bytes(parser_dir / name, digest)
            (scratch / "http_parser.h").write_bytes((parser_dir / "http_parser.h").read_bytes())
            (scratch / "esp_heap_caps.h").write_text("""#pragma once
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_DEFAULT 0U
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
void heap_caps_free(void *memory);
#define heap_caps_malloc(size, caps) malloc(size)
""", encoding="ascii")
            includes = (scratch, native / "include", native / "lib/include", PORT / "esp-idf",
                        native / "lib/mqtt_utils/include", PORT / "include", PORT / "host/include", HERE)
            command += ["-DXIAOTAI_MQTT_OWNER_BRIDGE=1", "-DXIAOTAI_MQTT_NATIVE_ID_RESERVATION=1",
                        "-DCONFIG_XIAOTAI_PORT_MQTT_NATIVE_OVERLAY=1",
                        "-DXIAOTAI_MQTT_NATIVE_QUEUE=1", "-DXIAOTAI_OWNER_REAL_LIFECYCLE=1",
                        "-DMQTT_SUPPORTED_FEATURE_EVENT_LOOP=1"]
            native_sources = (native / "lib/mqtt_msg.c", native / "lib/mqtt_outbox.c",
                              native / "lib/mqtt_utils/mqtt_utils.c")
            sources = (parser_dir / "http_parser.c", PORT / "esp-idf/mqtt.c", PORT / "common/mqtt_config.c",
                        PORT / "host/mem.c", PORT / "host/os.c")
        for include in includes:
            command.extend(("-I", str(include)))
        fixture = "mqtt_owner_port_test.c" if port else "mqtt_owner_integration_test.c"
        if port:
            objects = []
            for index, path in enumerate(native_sources):
                target = scratch / f"native-{index}.o"
                hooks = ("malloc", "calloc", "strdup", "free")
                flags = [f"-D{name}=fixture_lifecycle_{name}" for name in hooks]
                flags += ["-Dheap_caps_calloc=fixture_lifecycle_caps_calloc"]
                subprocess.run([*command, "-include", "mqtt_owner_lifecycle_provider.h", *flags,
                                "-c", str(path), "-o", str(target)], check=True, timeout=30)
                objects.append(target)
            sources += tuple(objects)
        subprocess.run([*command, str(HERE / fixture), *map(str, sources),
                        "-o", str(binary)], check=True, timeout=30)
        if port:
            unlocked = subprocess.run([*command, "-DCONFIG_MQTT_DISABLE_API_LOCKS=1", "-fsyntax-only",
                                       str(HERE / fixture)], capture_output=True, text=True, timeout=30)
            if not unlocked.returncode or "owner dispatch requires the native recursive API lock" not in unlocked.stderr:
                raise AssertionError("owner bridge accepted a native build without API locks")
            print("PASS: owner bridge rejects native API-lock-disabled configuration")
        for _ in range(repeat):
            subprocess.run([str(binary)], check=True, timeout=15)
            if not port:
                subprocess.run([str(binary), "reentry"], check=True, timeout=15)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mqtt-source", type=Path,
                        default=ROOT / "build/host-components/espressif__mqtt_1.1.0_fb18bc3b")
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--compile-db", type=Path, help="also syntax-check the complete integrated native source")
    parser.add_argument("--repeat", type=int, default=1, help="repeat the same built proof, 1..20")
    parser.add_argument("--port", action="store_true", help="link the actual port wrapper and opaque native owner API")
    parser.add_argument("--http-parser-dir", type=Path,
                        default=Path(os.environ["IDF_PATH"]) / "components/http_parser" if os.environ.get("IDF_PATH") else None,
                        help="pinned HTTP parser sources; defaults to IDF_PATH/components/http_parser")
    args = parser.parse_args()
    if not 1 <= args.repeat <= 20:
        parser.error("--repeat must be 1..20")
    if args.port and args.http_parser_dir is None:
        parser.error("--port requires --http-parser-dir or IDF_PATH")
    parser_dir = args.http_parser_dir.resolve() if args.http_parser_dir else None
    run(args.mqtt_source.resolve(), args.cc, args.repeat, args.port, parser_dir)
    if args.compile_db:
        if args.port:
            wrapper_syntax(args.mqtt_source.resolve(), args.compile_db.resolve())
        else:
            with TemporaryDirectory(prefix="xiaotai-owner-native-syntax-") as directory:
                candidate = Path(directory) / "native"
                write_candidate(args.mqtt_source.resolve(), candidate)
                native_plan_syntax(candidate, args.compile_db.resolve())


if __name__ == "__main__":
    main()
