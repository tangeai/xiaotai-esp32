# SPDX-License-Identifier: MIT
"""Real pinned native codec/outbox/synchronous paths, deterministic I/O barriers."""

import argparse
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
ROOT = PORT.parents[1]
sys.path.insert(0, str(PORT / "esp-idf"))
from mqtt_queue_overlay import patch_client_queue, patch_outbox_queue, patch_outbox_queue_header
from mqtt_id_overlay import overlay_msg_source, overlay_msg_header, overlay_outbox_source, overlay_outbox_header
from mqtt_native_fixture import write_compat, excerpt

FUNCTIONS = (
    "static inline void mqtt_reset_pending_message(",
    "static inline esp_err_t esp_mqtt_write(",
    "static void esp_mqtt_abort_connection(esp_mqtt_client_handle_t client)\n{",
    "static outbox_item_handle_t mqtt_enqueue(",
    "static int make_publish(",
    "static inline int mqtt_client_enqueue_publish(",
    "int esp_mqtt_client_publish(",
    "int esp_mqtt_client_subscribe_multiple(",
)

BASELINE = r"""
esp_err_t xiaotai_idf_mqtt_enqueue_connected(esp_mqtt_client_handle_t client, const char *topic,
 const char *data, int len, int qos, size_t wire, int *id)
{
    (void)topic; (void)data; (void)len; (void)qos; (void)wire;
    *id = 0;
    /* The previous candidate's exact first admission decision. It cannot
     * reach the native enqueue under the real API-lock network barrier. */
    if (xSemaphoreTakeRecursive(client->api_lock, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    MQTT_API_UNLOCK(client);
    return ESP_FAIL;
}
"""


def run(component, cc, baseline=False):
    raw = (component / "mqtt_client.c").read_bytes()
    for transform, path in ((patch_client_queue, component / "mqtt_client.c"),
                            (patch_outbox_queue, component / "lib/mqtt_outbox.c"),
                            (patch_outbox_queue_header, component / "lib/include/mqtt_outbox.h")):
        try:
            transform(path.read_bytes() + b"\n")
        except ValueError as error:
            assert "SHA-256" in str(error)
        else:
            raise AssertionError("accepted an unpinned native source")
    source = patch_client_queue(raw).decode("utf-8")
    with TemporaryDirectory(prefix="xiaotai-mqtt-queue-") as directory:
        scratch = Path(directory)
        write_compat(scratch)
        for name in ("freertos/FreeRTOS.h", "freertos/semphr.h"):
            (scratch / name).write_text('#pragma once\n#include "freertos/event_groups.h"\n', encoding="ascii")
        (scratch / "freertos/event_groups.h").write_text("""#pragma once
#include <pthread.h>
#include <stdbool.h>
typedef void *EventGroupHandle_t;
typedef void *TaskHandle_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
#define portMAX_DELAY (-1)
#define pdTRUE 1
int xSemaphoreTakeRecursive(SemaphoreHandle_t lock, int ticks);
void xSemaphoreGiveRecursive(SemaphoreHandle_t lock);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t lock);
""", encoding="ascii")
        (scratch / "esp_log.h").write_text("""#pragma once
void host_log(void);
#define ESP_LOGD(tag, ...) ((void)(tag), host_log())
#define ESP_LOGE(tag, ...) ((void)(tag), host_log())
#define ESP_LOGW(tag, ...) ((void)(tag), host_log())
#define ESP_LOGI(tag, ...) ((void)(tag), host_log())
#define ESP_MEM_CHECK(tag, value, action) do { (void)(tag); if (!(value)) { host_log(); action; } } while (0)
""", encoding="ascii")
        for name, transform in (("mqtt_msg.h", overlay_msg_header), ("mqtt_outbox.h", overlay_outbox_header)):
            raw_header = (component / "lib/include" / name).read_bytes()
            result = transform(raw_header)
            if name == "mqtt_outbox.h":
                result = patch_outbox_queue_header(raw_header, result)
            (scratch / name).write_bytes(result)
        (scratch / "mqtt_client_priv.h").write_bytes((component / "lib/include/mqtt_client_priv.h").read_bytes())
        (scratch / "mqtt_msg.c").write_bytes(overlay_msg_source((component / "lib/mqtt_msg.c").read_bytes()))
        box = (component / "lib/mqtt_outbox.c").read_bytes()
        (scratch / "mqtt_outbox.c").write_bytes(patch_outbox_queue(box, overlay_outbox_source(box)) +
                                               b"\nsize_t host_outbox_bookkeeping(void) { return sizeof(struct outbox_t); }\n")
        helper = BASELINE if baseline else (PORT / "esp-idf/mqtt_enqueue_connected.inc").read_text(encoding="utf-8")
        (scratch / "mqtt_queue_native.inc").write_text(
            '#include "mqtt_queue_client.inc"\n' + "\n".join(excerpt(source, start) for start in FUNCTIONS) + helper,
            encoding="utf-8")
        binary = scratch / "queue"
        command = [cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-Wno-unused-function",
                   "-D_GNU_SOURCE", "-pthread", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-fno-pie", "-no-pie", "-include", "mqtt_native_compat.h", "-Wl,--wrap=malloc", "-Wl,--wrap=calloc"]
        for include in (scratch, component / "include", component / "lib/include", PORT / "esp-idf", PORT / "include"):
            command.extend(("-I", str(include)))
        subprocess.run([*command, str(HERE / "mqtt_queue_native_test.c"), str(scratch / "mqtt_msg.c"),
                        str(scratch / "mqtt_outbox.c"), "-o", str(binary)], check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=15)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mqtt-source", type=Path, default=ROOT / "build/host-components/espressif__mqtt_1.1.0_fb18bc3b")
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--baseline", action="store_true", help="reproduce the prior zero-tick admission failure")
    args = parser.parse_args()
    run(args.mqtt_source.resolve(), args.cc, args.baseline)


if __name__ == "__main__":
    main()
