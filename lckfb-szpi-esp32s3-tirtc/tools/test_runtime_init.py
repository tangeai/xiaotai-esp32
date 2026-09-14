"""Run actual runtime initialization against each allocation failure."""
from pathlib import Path
import subprocess
import tempfile
from test_captive_portal_contract import function_body

root=Path(__file__).resolve().parents[1]
source=(root/"components/starter_runtime/src/starter_runtime.c").read_text(encoding="utf-8")
body=function_body(source,"starter_runtime_start")
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
typedef void *QueueHandle_t, *TaskHandle_t, *SemaphoreHandle_t;
typedef int esp_err_t;
typedef struct { char bytes[40]; } runtime_event_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG -1
#define ESP_ERR_NO_MEM -2
#define pdPASS 1
#define RUNTIME_QUEUE_DEPTH 12
#define RUNTIME_TASK_STACK_BYTES 24576
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static _Atomic(QueueHandle_t) s_queue;
static _Atomic(TaskHandle_t) s_task;
static _Atomic(SemaphoreHandle_t) s_product_mutex;
static char s_device_id[65];
static int failure, allocations, live, published, notifications;
static void *allocate(void) {
    assert(!s_queue && !s_task && !s_product_mutex);
    if (++allocations==failure) return NULL;
    ++live; return malloc(1);
}
static void release(void *p) { assert(p && live>0); --live; free(p); }
static QueueHandle_t xQueueCreate(int depth,size_t size) {
    assert(depth==12 && size==40); return allocate();
}
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return allocate(); }
static void vQueueDelete(QueueHandle_t p) { release(p); }
static void vSemaphoreDelete(SemaphoreHandle_t p) { release(p); }
static void runtime_task(void *p) { (void)p; assert(0 && "task must await notification"); }
static int xTaskCreateWithCaps(void (*fn)(void *),const char *name,unsigned bytes,
                              void *arg,int priority,TaskHandle_t *out,unsigned caps) {
    assert(fn==runtime_task && !strcmp(name,"starter_session") && !arg && priority==6);
    assert(bytes==24576 && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    *out=allocate(); return *out ? pdPASS : 0;
}
static void product_snapshot_reset(void) { assert(s_queue && s_task && s_product_mutex); ++published; }
static void callback(void) {}
#define on_tirtc_started callback
#define on_tirtc_connection callback
#define on_tirtc_command callback
#define on_tirtc_audio callback
#define on_platform_signal callback
#define on_platform_online callback
typedef struct { void (*on_started)(void),(*on_connection)(void),(*on_command)(void),(*on_audio)(void); } starter_tirtc_handlers_t;
static void starter_tirtc_set_handlers(const starter_tirtc_handlers_t *h) {
    assert(h->on_started==callback && published==1); ++published;
}
static void platform_client_set_signal_handler(void (*h)(void),void *p) {
    assert(h==callback && !p && published==2); ++published;
}
static void platform_client_set_online_handler(void (*h)(void),void *p) {
    assert(h==callback && !p && published==3); ++published;
}
static void xTaskNotifyGive(TaskHandle_t t) {
    assert(t==s_task && published==4 && live==3); ++notifications;
}
static esp_err_t starter_runtime_start(const char *device_id) { BODY }
int main(void) {
    assert(starter_runtime_start(NULL)==ESP_ERR_INVALID_ARG);
    assert(starter_runtime_start("")==ESP_ERR_INVALID_ARG);
    for(int i=1;i<=3;++i) {
        failure=i; allocations=0;
        assert(starter_runtime_start("test-device")==ESP_ERR_NO_MEM);
        assert(live==0 && !s_queue && !s_task && !s_product_mutex && !published && !notifications);
    }
    failure=0; allocations=0;
    assert(starter_runtime_start("test-device")==ESP_OK);
    assert(allocations==3 && live==3 && notifications==1);
    assert(starter_runtime_start("test-device")==ESP_OK && allocations==3 && notifications==1);
    release(s_queue); release(s_task); release(s_product_mutex);
    puts("PASS: runtime queue/mutex/task failures release only unpublished resources; retry and one-time handoff");
}
'''.replace("BODY",body)
with tempfile.TemporaryDirectory(prefix="runtime-init-") as tmp:
    c=Path(tmp)/"test.c"; exe=Path(tmp)/"test"
    c.write_text(harness,encoding="utf-8")
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-g",
                    "-fsanitize=address,undefined",str(c),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
