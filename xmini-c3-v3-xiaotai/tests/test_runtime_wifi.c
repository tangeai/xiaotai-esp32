// SPDX-License-Identifier: MIT
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../main/xmini_runtime.c"
#include "../components/xmini_settings/xmini_settings.c"
#include "../main/xmini_audio_control.h"

static jmp_buf s_yield;
static unsigned s_notified, s_acquires, s_releases, s_http_begins, s_http_ends, s_setup_requests, s_restarts;
static int s_business_init_error, s_create_error, s_release_error, s_acquire_error;
static int s_http_begin_error, s_http_end_error, s_setup_error;
static bool s_notify_timeout, s_maintenance_active, s_http_ready, s_http_held;
static xmini_audio_status_t s_audio;
static int s_main_handle, s_control_handle;
static TaskHandle_t s_current = &s_main_handle;
static int64_t s_nv_now;
static unsigned s_nv_sets, s_nv_commits;
static int s_nv_commit_error;
static bool s_nv_present;
static settings_record_t s_nv_record, s_nv_staged;
static unsigned s_audio_controls;
static int s_audio_control_error;

int64_t esp_timer_get_time(void) { return s_nv_now * 1000; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{ assert(!strcmp(name, "xmini_ui")); *handle = 1; return mode == NVS_READONLY && !s_nv_present ? ESP_ERR_NVS_NOT_FOUND : ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{ assert(handle == 1 && !strcmp(key, "audio") && *size == sizeof(s_nv_record)); memcpy(value, &s_nv_record, sizeof(s_nv_record)); return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{ assert(handle == 1 && !strcmp(key, "audio") && size == sizeof(s_nv_record)); ++s_nv_sets; memcpy(&s_nv_staged, value, size); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle)
{ assert(handle == 1); ++s_nv_commits; if (!s_nv_commit_error) { s_nv_record = s_nv_staged; s_nv_present = true; } return s_nv_commit_error; }
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }

QueueHandle_t xQueueCreateStatic(unsigned capacity, unsigned size, uint8_t *buffer, StaticQueue_t *storage)
{ *storage = (StaticQueue_t){.capacity = capacity, .size = size, .buffer = buffer}; return storage; }
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    (void)wait;
    if (queue->count == queue->capacity) return pdFALSE;
    memcpy(queue->buffer + ((queue->head + queue->count) % queue->capacity) * queue->size, item, queue->size);
    ++queue->count; return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    (void)wait;
    if (!queue || !queue->count) return pdFALSE;
    memcpy(item, queue->buffer + queue->head * queue->size, queue->size);
    queue->head = (queue->head + 1) % queue->capacity; --queue->count; return pdTRUE;
}
unsigned uxQueueMessagesWaiting(QueueHandle_t queue) { return queue->count; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return s_current; }
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, unsigned stack, void *context, unsigned priority, TaskHandle_t *task)
{
    assert(!strcmp(name, "xmini_control") && stack == 10240 && priority == 4);
    if (s_create_error) return pdFALSE;
    *task = &s_control_handle;
    s_current = &s_control_handle;
    if (!setjmp(s_yield)) function(context);
    s_current = &s_main_handle;
    return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task == &s_main_handle); ++s_notified; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{ (void)clear; (void)wait; return s_notify_timeout ? 0 : s_notified; }
void vTaskDelete(TaskHandle_t task) { (void)task; longjmp(s_yield, 1); }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { return task ? 1234 : 0; }
int xmini_business_init(const xmini_business_callbacks_t *callbacks) { assert(callbacks->receive); return s_business_init_error; }
void xmini_business_process(void) { longjmp(s_yield, 1); }
int xmini_business_maintenance(bool active)
{
    if (active) {
        ++s_acquires;
        if (!s_acquire_error) s_maintenance_active = true;
        return s_acquire_error;
    }
    ++s_releases;
    if (!s_release_error) s_maintenance_active = false;
    return s_release_error;
}
esp_err_t platform_http_exclusive_begin(uint32_t *ticket)
{
    ++s_http_begins;
    if (s_http_begin_error) return s_http_begin_error;
    assert(s_maintenance_active && !s_http_held);
    s_http_held = true; *ticket = 123; return ESP_OK;
}
bool platform_http_exclusive_ready(uint32_t ticket) { return ticket == 123 && s_http_held && s_http_ready; }
esp_err_t platform_http_exclusive_end(uint32_t ticket)
{
    ++s_http_ends;
    assert(ticket == 123 && s_http_held);
    if (s_http_end_error) return s_http_end_error;
    s_http_held = false; return ESP_OK;
}
esp_err_t platform_request_wifi_provisioning(uint32_t ticket)
{
    assert(platform_http_exclusive_ready(ticket) && s_maintenance_active);
    ++s_setup_requests; return s_setup_error;
}
void esp_restart(void) { ++s_restarts; }
int xmini_business_send_audio(uint32_t generation, const uint8_t *packet, size_t length, uint32_t pts)
{ (void)generation; (void)packet; (void)length; (void)pts; return 0; }
xmini_audio_status_t xmini_audio_status(void) { return s_audio; }
bool xmini_audio_receive(uint32_t generation, const uint8_t *packet, size_t length)
{ (void)generation; (void)packet; (void)length; return true; }
int xmini_audio_request(xmini_audio_request_t request)
{
    ++s_audio_controls;
    if (s_audio_control_error) return s_audio_control_error;
    if (request.kind == XMINI_AUDIO_VOLUME) s_audio.volume = request.value;
    if (request.kind == XMINI_AUDIO_GAIN) s_audio.gain_db = request.value;
    return ESP_OK;
}
int xmini_audio_stream_start(uint32_t generation, xmini_audio_tx_t transmit, void *context)
{ (void)generation; (void)transmit; (void)context; return 0; }
int xmini_audio_stream_stop(uint32_t generation) { (void)generation; return 0; }
bool xmini_audio_stream_busy(uint32_t generation) { return s_audio.busy && s_audio.generation == generation; }
void xmini_audio_stream_flush(uint32_t generation) { (void)generation; }

static void reset(void)
{
    s_task = NULL; s_init_error = 0; s_wifi_queue = NULL; s_http_ticket = 0; s_settings_held = false;
    atomic_store(&s_ready, false); atomic_store(&s_wifi_error, 0); atomic_store(&s_wifi_phase, XMINI_WIFI_SETUP_IDLE);
    s_notified = s_acquires = s_releases = s_http_begins = s_http_ends = s_setup_requests = s_restarts = 0;
    s_business_init_error = s_create_error = s_release_error = s_acquire_error = 0;
    s_http_begin_error = s_http_end_error = s_setup_error = 0;
    s_notify_timeout = s_maintenance_active = s_http_held = false;
    s_http_ready = true;
    memset(&s_state, 0, sizeof(s_state)); s_loading = s_quiesced = false;
    s_nv_now = s_nv_sets = s_nv_commits = 0; s_nv_commit_error = 0; s_nv_present = false;
    int volume, gain;
    assert(xmini_settings_load(&volume, &gain) == ESP_OK && volume == 40 && gain == 24);
    s_audio = (xmini_audio_status_t){.ready = true, .volume = volume, .gain_db = gain};
    s_audio_controls = 0; s_audio_control_error = ESP_OK;
}
static void request(void) { assert(xmini_runtime_wifi_setup() == ESP_OK); wifi_setup_process(); }
static void cancel(void) { assert(xmini_runtime_wifi_cancel() == ESP_OK); wifi_setup_process(); }
static void not_ready(void)
{
    assert(!xmini_runtime_ready());
    assert(xmini_runtime_wifi_setup() == ESP_ERR_INVALID_STATE);
    assert(xmini_runtime_wifi_cancel() == ESP_ERR_INVALID_STATE);
}
static void test_setup_pending_settings(void)
{
    reset(); assert(xmini_runtime_init() == ESP_OK);
    assert(xmini_settings_schedule(65, 24) == ESP_OK);
    request();
    assert(xmini_settings_status().pending && !s_nv_commits);
    assert(!s_setup_requests && !s_restarts);
    assert(s_settings_held && xmini_settings_status().quiesced);
    assert(xmini_settings_schedule(75, 30) == ESP_ERR_INVALID_STATE);
    assert(xmini_settings_status().volume == 65);
    s_nv_now = 999; xmini_settings_process(); wifi_setup_process();
    assert(!s_nv_sets && !s_restarts);
    s_nv_now = 1000; xmini_settings_process(); wifi_setup_process();
    assert(s_nv_commits == 1 && s_nv_record.volume == 65);
    assert(xmini_settings_status().saved && s_setup_requests == 1 && s_restarts == 1);

    reset(); assert(xmini_runtime_init() == ESP_OK);
    assert(xmini_settings_schedule(65, 24) == ESP_OK);
    request(); cancel();
    assert(!s_settings_held && !xmini_settings_status().quiesced && !s_restarts);
    assert(xmini_settings_schedule(75, 30) == ESP_OK);

    reset(); assert(xmini_runtime_init() == ESP_OK);
    assert(xmini_settings_schedule(65, 24) == ESP_OK);
    request(); s_nv_commit_error = ESP_FAIL; s_nv_now = 1000;
    xmini_settings_process(); wifi_setup_process();
    assert(xmini_runtime_wifi_phase() == XMINI_WIFI_SETUP_ERROR_HELD && xmini_runtime_wifi_error() == ESP_FAIL);
    assert(s_http_held && s_maintenance_active && xmini_settings_status().quiesced);
    for (unsigned i = 0; i < 100; ++i) { ++s_nv_now; xmini_settings_process(); wifi_setup_process(); }
    assert(s_nv_sets == 1 && s_nv_commits == 1 && !s_setup_requests && !s_restarts);
    assert(xmini_settings_schedule(65, 24) == ESP_ERR_INVALID_STATE);
    cancel();
    assert(!s_settings_held && !xmini_settings_status().quiesced && xmini_settings_status().error == ESP_FAIL);
    assert(xmini_settings_schedule(65, 24) == ESP_OK);
    request();
    assert(xmini_runtime_wifi_phase() == XMINI_WIFI_SETUP_WAITING && !s_restarts);
    assert(xmini_settings_status().error == ESP_FAIL && xmini_settings_status().attempt_pending);
    s_nv_commit_error = 0; s_nv_now += 999; xmini_settings_process(); wifi_setup_process();
    assert(s_nv_commits == 1 && !s_restarts);
    ++s_nv_now; xmini_settings_process(); wifi_setup_process();
    assert(s_nv_commits == 2 && xmini_settings_status().saved);
    assert(s_setup_requests == 1 && s_restarts == 1);
}

static void test_settings_audio_admission(void)
{
    reset();
    xmini_audio_request_t request = {.kind = XMINI_AUDIO_VOLUME, .value = 65};
    assert(xmini_settings_quiesce_begin() == ESP_OK);
    assert(xmini_request_audio(request) == ESP_ERR_INVALID_STATE);
    assert(!s_audio_controls && s_audio.volume == 40 && xmini_settings_status().volume == 40);
    xmini_settings_quiesce_end();
    assert(xmini_request_audio(request) == ESP_OK && s_audio.volume == 65);
    assert(xmini_settings_status().volume == 65 && xmini_settings_status().pending);
    unsigned revision = xmini_settings_status().revision;
    assert(xmini_request_audio(request) == ESP_OK && xmini_settings_status().revision == revision);
    s_audio_control_error = ESP_FAIL; request.value = 75;
    assert(xmini_request_audio(request) == ESP_FAIL && s_audio.volume == 65);
    assert(xmini_settings_status().volume == 75 && xmini_settings_status().pending);
    s_nv_now = 1000; xmini_settings_process();
    assert(s_nv_record.volume == 75 && xmini_settings_status().saved);
    /* Durable preference is accepted independently; the failed live hardware
     * application remains an error and is never reported as applied. */
    s_audio.ready = false; request.value = 85;
    assert(xmini_request_audio(request) == ESP_ERR_INVALID_STATE);
    assert(xmini_settings_status().volume == 75);
}
int main(void)
{
    reset(); not_ready();
    s_business_init_error = ESP_FAIL;
    assert(xmini_runtime_init() == ESP_FAIL); not_ready();
    reset(); s_create_error = ESP_FAIL;
    assert(xmini_runtime_init() == ESP_ERR_NO_MEM); not_ready();
    reset(); s_notify_timeout = true;
    assert(xmini_runtime_init() == ESP_ERR_TIMEOUT); not_ready();

    reset(); assert(xmini_runtime_init() == ESP_OK);
    assert(xmini_runtime_wifi_setup() == ESP_OK);
    assert(xmini_runtime_wifi_cancel() == ESP_ERR_INVALID_STATE);
    assert(xmini_runtime_wifi_setup() == ESP_ERR_INVALID_STATE);
    assert(!s_acquires && !s_http_begins && !s_setup_requests);
    wifi_setup_process();
    assert(s_maintenance_active && s_http_held && s_setup_requests == 1 && s_restarts == 1);
    assert(xmini_runtime_wifi_phase() == XMINI_WIFI_SETUP_REBOOTING);
    for (unsigned i = 0; i < 10; ++i) wifi_setup_process();
    assert(s_setup_requests == 1 && s_restarts == 1);

    reset(); assert(xmini_runtime_init() == ESP_OK); s_http_ready = false;
    request();
    assert(xmini_runtime_wifi_phase() == XMINI_WIFI_SETUP_WAITING && s_http_held && !s_setup_requests);
    request();
    assert(s_http_begins == 1 && s_acquires == 1 && xmini_runtime_wifi_error() == ESP_ERR_INVALID_STATE);
    for (unsigned i = 0; i < 10; ++i) wifi_setup_process();
    assert(!s_releases && !s_setup_requests);
    cancel();
    assert(!s_http_held && !s_maintenance_active && xmini_runtime_wifi_phase() == XMINI_WIFI_SETUP_IDLE);
    assert(!s_setup_requests && !s_restarts);
    request(); s_http_ready = true; wifi_setup_process();
    assert(s_restarts == 1 && s_setup_requests == 1 && !xmini_runtime_wifi_error());

    reset(); assert(xmini_runtime_init() == ESP_OK); s_acquire_error = ESP_FAIL;
    request();
    assert(!s_http_begins && !s_http_held && !s_maintenance_active && !s_restarts);
    assert(xmini_runtime_wifi_error() == ESP_FAIL);
    reset(); assert(xmini_runtime_init() == ESP_OK); s_http_begin_error = ESP_FAIL;
    request();
    assert(!s_http_held && !s_maintenance_active && !s_restarts && s_releases == 1);

    reset(); assert(xmini_runtime_init() == ESP_OK); s_setup_error = ESP_FAIL;
    request();
    assert(s_setup_requests == 1 && !s_restarts && !s_http_held && !s_maintenance_active);
    assert(xmini_runtime_wifi_error() == ESP_FAIL);
    reset(); assert(xmini_runtime_init() == ESP_OK); s_http_ready = false;
    request(); s_http_end_error = ESP_FAIL; cancel();
    assert(s_http_held && s_maintenance_active && xmini_runtime_wifi_phase() == XMINI_WIFI_SETUP_ERROR_HELD);
    assert(s_settings_held && xmini_settings_status().quiesced);
    unsigned ends = s_http_ends;
    for (unsigned i = 0; i < 10; ++i) wifi_setup_process();
    assert(s_http_ends == ends && !s_restarts);
    request(); assert(s_acquires == 1 && s_http_begins == 1);
    s_http_end_error = 0; cancel();
    assert(!s_http_held && !s_maintenance_active && xmini_runtime_wifi_phase() == XMINI_WIFI_SETUP_IDLE);
    assert(!s_settings_held && !xmini_settings_status().quiesced);

    reset(); assert(xmini_runtime_init() == ESP_OK); s_http_ready = false;
    request(); s_release_error = ESP_FAIL; cancel();
    assert(!s_http_held && s_maintenance_active && xmini_runtime_wifi_phase() == XMINI_WIFI_SETUP_ERROR_HELD);
    assert(s_settings_held && xmini_settings_status().quiesced);
    ends = s_http_ends;
    for (unsigned i = 0; i < 10; ++i) wifi_setup_process();
    assert(s_http_ends == ends && s_releases == 1);
    s_release_error = 0; cancel();
    assert(s_http_ends == ends && !s_maintenance_active);
    assert(!s_settings_held && !xmini_settings_status().quiesced);
    cancel(); assert(xmini_runtime_wifi_error() == ESP_ERR_INVALID_STATE);
    test_setup_pending_settings();
    test_settings_audio_admission();
    puts("runtime Wi-Fi: init admission, bounded intents, true HTTP drain, explicit restart, cancel and failed-release ownership passed");
    return 0;
}
