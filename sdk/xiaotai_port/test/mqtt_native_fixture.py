# SPDX-License-Identifier: MIT
"""Thin host compatibility providers shared by native MQTT conformance proofs."""

from pathlib import Path
import sys
from textwrap import dedent


PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT / "esp-idf"))
from mqtt_overlay_common import locked_bytes, native_function as excerpt  # noqa: E402,F401
from mqtt_id_overlay import PINS  # noqa: E402

MSG_SHA256 = PINS["msg_source"]
OUTBOX_SHA256 = PINS["outbox_source"]


def write_compat(scratch):
    headers = {
        "esp_err.h": """
            #include "xiaotai/port/err.h"
            typedef xiaotai_err_t esp_err_t;
            #define ESP_OK XIAOTAI_OK
            #define ESP_FAIL XIAOTAI_FAIL
            #define ESP_ERR_NO_MEM XIAOTAI_ERR_NO_MEM
            #define ESP_ERR_INVALID_ARG XIAOTAI_ERR_INVALID_ARG
            #define ESP_ERR_INVALID_STATE XIAOTAI_ERR_INVALID_STATE
            #define ESP_ERR_TIMEOUT XIAOTAI_ERR_TIMEOUT
            #define ESP_ERR_NOT_SUPPORTED XIAOTAI_ERR_NOT_SUPPORTED
        """,
        "esp_event.h": """
            #include <stdint.h>
            typedef void *esp_event_loop_handle_t;
            typedef const char *esp_event_base_t;
            typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
            #define ESP_EVENT_DECLARE_BASE(name) extern esp_event_base_t name
        """,
        "esp_transport.h": """
            #include <stdbool.h>
            typedef struct esp_transport_item_t *esp_transport_handle_t;
            typedef void *esp_transport_list_handle_t;
            typedef struct { bool keep_alive_enable; } esp_transport_keep_alive_t;
        """,
        "sdkconfig.h": """
            #define CONFIG_MQTT_PROTOCOL_311 1
            #define CONFIG_MQTT_MSG_ID_INCREMENTAL 1
        """,
        "freertos/event_groups.h": """
            #include <assert.h>
            #include <errno.h>
            #include <pthread.h>
            typedef void *EventGroupHandle_t;
            typedef void *TaskHandle_t;
            typedef pthread_mutex_t *SemaphoreHandle_t;
            #define portMAX_DELAY (-1)
            #define pdTRUE 1
            static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t lock, int ticks)
            {
                assert(ticks == 0 || ticks == portMAX_DELAY);
                const int rc = ticks ? pthread_mutex_lock(lock) : pthread_mutex_trylock(lock);
                assert(rc == 0 || (!ticks && rc == EBUSY));
                return rc == 0 ? pdTRUE : 0;
            }
            static inline void xSemaphoreGiveRecursive(SemaphoreHandle_t lock)
            {
                assert(!pthread_mutex_unlock(lock));
            }
        """,
        "esp_heap_caps.h": """
            #include <stdlib.h>
            #define MALLOC_CAP_DEFAULT (0)
            #define heap_caps_malloc(size, caps) malloc(size)
            #define heap_caps_calloc(count, size, caps) calloc(count, size)
        """,
        "esp_log.h": """
            #define ESP_LOGD(tag, ...) ((void)(tag))
            #define ESP_LOGE(tag, ...) ((void)(tag))
            #define ESP_LOGW(tag, ...) ((void)(tag))
            #define ESP_MEM_CHECK(tag, value, action) do { \
                (void)(tag); if (!(value)) { action; } \
            } while (0)
        """,
        "mqtt_native_compat.h": """
            #include "esp_log.h"
            #include <stddef.h>
            #include <stdint.h>
            #include <sys/queue.h>
            #ifndef STAILQ_FOREACH_SAFE
            #define STAILQ_FOREACH_SAFE(var, head, field, next_value) \
                for ((var) = STAILQ_FIRST(head); (var) && \
                    ((next_value) = STAILQ_NEXT(var, field), 1); (var) = (next_value))
            #endif
        """,
        **dict.fromkeys(("esp_transport_tcp.h", "esp_transport_ssl.h", "esp_transport_ws.h", "http_parser.h"), ""),
    }
    for name, body in headers.items():
        path = scratch / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#pragma once\n" + dedent(body), encoding="ascii")
