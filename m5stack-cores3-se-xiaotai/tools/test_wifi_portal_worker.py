#!/usr/bin/env python3
"""Exercise portal request/completion ordering without running on sys_evt."""
from pathlib import Path
import subprocess
import tempfile
from test_captive_portal_contract import function_body

root = Path(__file__).resolve().parents[1]
source = (root / "components/wifi_manager/src/wifi_manager.c").read_text(encoding="utf-8")
functions = source[source.index("static esp_err_t request_provisioning("):
                   source.index("static esp_err_t configure_station_dns(")]
event = function_body(source, "wifi_event")
control = function_body(source, "wifi_control_event")
retry = function_body(source, "retry_schedule")
assert "start_provisioning(" not in event + control + retry
assert "stop_provisioning(" not in event + control + retry
assert "portal_scan_cancel(" not in function_body(source, "stop_provisioning")
assert "provisioning_worker_step();" in function_body(source, "signal_task")
assert 'xTaskCreate(signal_task, "wifi_signal", 3072' in source
assert "s_portal_enabled" in function_body(source, "portal_socket_allowed")

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE -1
#define WIFI_MANAGER_CONTROL 1
#define WIFI_CONTROL_PORTAL_DONE 3
#define PORTAL_SCAN_IDLE 0
#define portMAX_DELAY UINT32_MAX
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static int s_signal_task, s_signal_lock, locked, notifications;
#define taskENTER_CRITICAL(lock) do { assert((lock)==&s_signal_lock && !locked); locked=1; } while (0)
#define taskEXIT_CRITICAL(lock) do { assert((lock)==&s_signal_lock && locked); locked=0; } while (0)
static void xTaskNotifyGive(int task) { assert(task == 1); ++notifications; }
static atomic_bool s_provisioning, s_portal_enabled, s_control_pending;
static atomic_int s_control_error;
static struct { uint32_t revision; bool manual; } s_portal_request;
typedef struct { uint32_t revision; esp_err_t error; unsigned stack_free; bool enabled, manual; } portal_completion_t;
static portal_completion_t queued[32];
static unsigned queue_count, starts, stops, scan_cancels, change;
static int portal_error, post_error;
static esp_err_t request_provisioning(bool enabled, bool manual);
static void portal_scan_cancel(int state) { assert(state == PORTAL_SCAN_IDLE); ++scan_cancels; }
static int start_provisioning(void) {
    assert(!locked); ++starts;
    if (change == 1) { change=0; assert(request_provisioning(false, false) == 0); }
    if (!portal_error) s_provisioning=true;
    return portal_error;
}
static int stop_provisioning(void) {
    assert(!locked); ++stops; s_provisioning=false;
    if (change == 2) { change=0; assert(request_provisioning(true, true) == 0); }
    return portal_error;
}
static unsigned uxTaskGetStackHighWaterMark(void *task) { assert(!task); return 1234; }
static int esp_event_post(int base, int id, const void *data, size_t size, unsigned ticks) {
    assert(!locked && base==WIFI_MANAGER_CONTROL && id==WIFI_CONTROL_PORTAL_DONE);
    assert(size == sizeof(portal_completion_t) && ticks==portMAX_DELAY);
    if (post_error) return post_error;
    assert(queue_count < 32); queued[queue_count++] = *(const portal_completion_t *)data;
    return 0;
}
FUNCTIONS
static void complete_latest(void) {
    assert(queue_count); provisioning_complete(&queued[--queue_count]);
}
int main(void) {
    assert(request_provisioning(true, true)==ESP_ERR_INVALID_STATE);
    assert(!s_portal_request.revision && !notifications && !starts);
    s_signal_task=1; s_control_pending=true;
    assert(request_provisioning(true, true)==0);
    assert(notifications==1 && !starts && !stops && s_control_pending);
    provisioning_worker_step(); assert(starts==1 && s_provisioning && s_control_pending);
    complete_latest(); assert(!s_control_pending && s_control_error==0);
    provisioning_worker_step(); assert(starts==1 && !queue_count);
    /* Stop revokes writes synchronously, but joins services only on the worker. */
    assert(request_provisioning(false, false)==0);
    assert(!s_portal_enabled && !s_provisioning && !stops && scan_cancels==1);
    provisioning_worker_step(); complete_latest(); assert(stops==1);
    /* A stop received during startup must defeat the stale completion. */
    s_control_pending=true; s_control_error=-12;
    assert(request_provisioning(true, true)==0); change=1;
    provisioning_worker_step(); assert(!s_portal_enabled && s_provisioning);
    complete_latest(); assert(s_control_pending && s_control_error==-12);
    provisioning_worker_step(); complete_latest(); assert(!s_provisioning && !s_control_pending);
    /* Coalesce to the latest request, retaining its manual completion contract. */
    assert(request_provisioning(false, false)==0);
    assert(request_provisioning(true, true)==0);
    unsigned old_stops=stops;
    provisioning_worker_step(); complete_latest();
    assert(stops==old_stops && s_provisioning && !s_control_pending);
    portal_error=-22; s_control_pending=true;
    assert(request_provisioning(true, true)==0);
    provisioning_worker_step(); assert(s_control_pending);
    complete_latest(); assert(!s_control_pending && s_control_error==-22);
    /* Completion allocation failure cannot leave manual controls stuck forever. */
    portal_error=0; post_error=-23; s_control_pending=true;
    assert(request_provisioning(true, true)==0);
    provisioning_worker_step(); assert(!s_control_pending && s_control_error==-23 && !queue_count);
    post_error=0; s_control_pending=true;
    assert(request_provisioning(true, true)==0);
    provisioning_worker_step(); complete_latest(); assert(!s_control_pending && s_control_error==0);
    /* An older failed completion cannot cancel a newer accepted request. */
    portal_error=-24;
    assert(request_provisioning(false, false)==0);
    change=2; s_control_pending=true;
    provisioning_worker_step(); complete_latest();
    assert(s_portal_enabled && s_control_pending && s_control_error==0);
    portal_error=0; provisioning_worker_step(); complete_latest();
    assert(s_provisioning && !s_control_pending && s_control_error==0);
    provisioning_complete(NULL);
    puts("PASS: portal off sys_evt; no new stack; latest intent; immediate revocation; delayed/manual completion; stale/OOM/error/retry paths");
}
'''.replace("FUNCTIONS", functions).replace("s_signal_task == NULL", "s_signal_task == 0")
with tempfile.TemporaryDirectory(prefix="xiaotai-portal-worker-") as tmp:
    path = Path(tmp)
    (path / "worker.c").write_text(harness)
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "worker.c"), "-o", str(path / "worker")], check=True)
    subprocess.run([str(path / "worker")], check=True)
