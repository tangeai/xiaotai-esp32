#!/usr/bin/env python3
"""Run actual resource-owner C functions against failure-injectable host APIs.

Host-only: no serial ports, credentials, network or firmware changes.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"(?m)^(?:static )?(?:bool|void|esp_err_t) " + name + r"\(", source)
    assert match, name
    begin = source.index("{", match.start())
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def run_case(name, body):
    with tempfile.TemporaryDirectory(prefix="xiaotai-resource-") as directory:
        source = Path(directory) / "test.c"
        binary = Path(directory) / "test"
        source.write_text(body, encoding="utf-8")
        subprocess.run([os.environ.get("CC", "gcc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-Wno-unused-parameter", "-Wno-unused-variable",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS:", name, flush=True)


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <setjmp.h>
typedef int esp_err_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef unsigned StackType_t;
typedef unsigned StaticTask_t;
typedef unsigned StaticQueue_t;
#define DRAM_ATTR
#define ESP_OK 0
#define ESP_ERR_NO_MEM 257
#define ESP_ERR_INVALID_ARG 258
#define ESP_ERR_INVALID_STATE 259
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
'''


def runtime_case():
    text = (ROOT / "components/starter_runtime/src/starter_runtime.c").read_text(encoding="utf-8")
    assert not re.search(r"suspend_mqtt|resume_mqtt|esp_mqtt_client_(stop|destroy)", text)
    body = COMMON + r'''
#define CONFIG_IDF_TARGET_ESP32P4 1
#define RUNTIME_QUEUE_DEPTH 12
#define RUNTIME_TASK_STACK_BYTES 24576
#define EXTERNAL_CONNECT_RESERVE_BYTES (17U*1024U)
typedef struct { int value; } runtime_event_t;
typedef struct { void (*on_started)(void),(*on_connection)(void),(*on_command)(void),
    (*on_audio)(void),(*on_video)(void),(*on_key_frame)(void); } starter_tirtc_handlers_t;
static char s_device_id[65];
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_product_mutex;
static void *s_external_connect_reserve;
static int64_t s_connect_reserve_retry_ms;
static int fail_at, allocated, created, callbacks, fail_heap;
static void runtime_task(void *arg) {}
static void on_tirtc_started(void) {}
static void on_tirtc_connection(void) {}
static void on_tirtc_command(void) {}
static void on_tirtc_audio(void) {}
static void on_tirtc_video(void) {}
static void on_tirtc_key_frame(void) {}
static void on_platform_signal(void) {}
static void on_platform_online(void) {}
static void *allocate(void) { if (++created==fail_at) return NULL; ++allocated; return malloc(16); }
static void release(void *p) { assert(p); --allocated; free(p); }
static void *xQueueCreate(int depth,size_t size) { assert(depth==12); return allocate(); }
static void *xSemaphoreCreateMutex(void) { return allocate(); }
static void vQueueDelete(void *p) { release(p); }
static void vSemaphoreDelete(void *p) { release(p); }
static void product_snapshot_reset(void) { assert(s_queue && s_product_mutex); }
static int xTaskCreateWithCaps(void (*fn)(void *),const char *name,unsigned bytes,
    void *arg,int pri,void **out,unsigned caps) {
    assert(bytes==24576 && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    *out=allocate(); return *out ? pdPASS : 0;
}
static void starter_tirtc_set_handlers(const starter_tirtc_handlers_t *h) {
    assert(s_task && s_queue && s_product_mutex); ++callbacks;
}
static void platform_client_set_signal_handler(void (*f)(void),void *arg) { ++callbacks; }
static void platform_client_set_online_handler(void (*f)(void),void *arg) { ++callbacks; }
static int64_t now_ms(void) { return 1000; }
static void *heap_caps_malloc(size_t size,unsigned caps) {
    assert(size==17408 && caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    return fail_heap ? NULL : malloc(size);
}
static void heap_caps_free(void *p) { free(p); }
'''
    body += function(text, "starter_runtime_arm_external_connect_reserve")
    body += function(text, "prepare_external_connect")
    body += function(text, "restore_external_connect_reserve")
    body += function(text, "starter_runtime_start")
    body += r'''
int main(void) {
    assert(starter_runtime_start(NULL)==ESP_ERR_INVALID_ARG);
    for (fail_at=1;fail_at<=3;++fail_at) {
        created=0;
        assert(starter_runtime_start("test-device")==ESP_ERR_NO_MEM);
        assert(!s_queue && !s_product_mutex && !s_task && allocated==0 && callbacks==0);
    }
    fail_at=0; created=0;
    assert(starter_runtime_start("test-device")==ESP_OK && callbacks==3);
    assert(starter_runtime_start("test-device")==ESP_OK && created==3 && callbacks==3);
    assert(!prepare_external_connect());
    for (unsigned i=0;i<1000;++i) {
        assert(starter_runtime_arm_external_connect_reserve()==ESP_OK);
        assert(prepare_external_connect() && !s_external_connect_reserve && !s_connect_reserve_retry_ms);
        assert(!prepare_external_connect()); /* no overlapping reserve owner */
        fail_heap=true; restore_external_connect_reserve();
        assert(!s_external_connect_reserve && s_connect_reserve_retry_ms==6000);
        fail_heap=false; restore_external_connect_reserve();
        assert(s_external_connect_reserve && !s_connect_reserve_retry_ms);
    }
    free(s_external_connect_reserve); release(s_task); release(s_product_mutex); release(s_queue);
    assert(allocated==0);
    puts("runtime: 3 init failures, idempotent retry, 1000 reserve cycles and rearm failures OK");
}
'''
    run_case("runtime owner", body)


if __name__ == "__main__":
    runtime_case()
