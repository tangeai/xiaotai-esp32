// SPDX-License-Identifier: MIT
#include "xmini_runtime.h"
#include "xmini_audio.h"
#include "xmini_business.h"
#include "xmini_settings.h"
#include "demo_platform.h"
#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static TaskHandle_t s_task;
static int s_init_error;
static atomic_bool s_ready;
static QueueHandle_t s_wifi_queue;
static StaticQueue_t s_wifi_queue_storage;
static uint8_t s_wifi_queue_buffer[1];
static uint32_t s_http_ticket;
static bool s_settings_held;
static atomic_int s_wifi_error;
static atomic_int s_wifi_phase;

static int maintenance_release(void)
{
    int error = s_http_ticket ? platform_http_exclusive_end(s_http_ticket) : ESP_OK;
    if (!error) {
        s_http_ticket = 0;
        error = xmini_business_maintenance(false);
        if (!error && s_settings_held) {
            xmini_settings_quiesce_end();
            s_settings_held = false;
        }
    }
    atomic_store(&s_wifi_phase, error ? XMINI_WIFI_SETUP_ERROR_HELD : XMINI_WIFI_SETUP_IDLE);
    return error;
}

static int maintenance_begin(void)
{
    if (atomic_load(&s_wifi_phase) != XMINI_WIFI_SETUP_IDLE) return ESP_ERR_INVALID_STATE;
    int error = xmini_business_maintenance(true);
    if (error) return error;
    error = xmini_settings_quiesce_begin();
    if (!error) {
        s_settings_held = true;
        error = platform_http_exclusive_begin(&s_http_ticket);
    }
    if (!error) atomic_store(&s_wifi_phase, XMINI_WIFI_SETUP_WAITING);
    else {
        int released = maintenance_release();
        if (released) error = released;
    }
    return error;
}

static void wifi_setup_process(void)
{
    uint8_t action;
    if (xQueueReceive(s_wifi_queue, &action, 0) == pdTRUE) {
        int error;
        if (action == 1) error = maintenance_begin();
        else if (atomic_load(&s_wifi_phase) == XMINI_WIFI_SETUP_WAITING ||
                 atomic_load(&s_wifi_phase) == XMINI_WIFI_SETUP_ERROR_HELD) {
            error = maintenance_release();
        } else error = ESP_ERR_INVALID_STATE;
        atomic_store(&s_wifi_error, error);
        if (error) ESP_LOGW("xmini_runtime", "Wi-Fi setup action=%u rejected: %d", action, error);
    }
    if (atomic_load(&s_wifi_phase) == XMINI_WIFI_SETUP_WAITING) {
        xmini_settings_status_t settings = xmini_settings_status();
        if ((settings.error && !settings.attempt_pending) || !settings.loaded) {
            atomic_store(&s_wifi_error, settings.error ? settings.error : ESP_ERR_INVALID_STATE);
            atomic_store(&s_wifi_phase, XMINI_WIFI_SETUP_ERROR_HELD);
            ESP_LOGE("xmini_runtime", "Wi-Fi setup held: settings not saved, explicit cancel/retry required");
            return;
        }
        if (!settings.saved || settings.pending || !platform_http_exclusive_ready(s_http_ticket)) return;
        int error = platform_request_wifi_provisioning(s_http_ticket);
        if (!error) {
            atomic_store(&s_wifi_error, ESP_OK);
            atomic_store(&s_wifi_phase, XMINI_WIFI_SETUP_REBOOTING);
            ESP_LOGI("xmini_runtime", "explicit Wi-Fi setup accepted; restarting into provisioning");
            esp_restart();
            return;
        }
        int released = maintenance_release();
        atomic_store(&s_wifi_error, released ? released : error);
    }
}

static bool transmit(uint32_t generation, const uint8_t *packet, size_t length, uint64_t pts_ms, void *context)
{
    (void)context;
    return xmini_business_send_audio(generation, packet, length, (uint32_t)pts_ms) == 0;
}

static bool receive(uint32_t generation, uint8_t stream, uint32_t pts, const uint8_t *packet,
                    size_t length, void *context)
{
    (void)stream;
    (void)pts;
    (void)context;
    return xmini_audio_receive(generation, packet, length);
}

static int media_start(uint32_t generation, uint8_t stream, void *context)
{
    (void)stream;
    (void)context;
    xmini_audio_status_t audio = xmini_audio_status();
    if (audio.owner == XMINI_AUDIO_RTC && audio.generation == generation && audio.error) return ESP_FAIL;
    if (xmini_audio_stream_busy(generation)) return audio.device_open ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (audio.busy) {
        if (audio.owner == XMINI_AUDIO_GUIDANCE)
            xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_STOP});
        return ESP_ERR_INVALID_STATE;
    }
    int error = xmini_audio_stream_start(generation, transmit, NULL);
    return error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
}

static int media_stop(uint32_t generation, void *context)
{
    (void)context;
    return xmini_audio_stream_busy(generation) ? xmini_audio_stream_stop(generation) : ESP_OK;
}

static bool media_busy(uint32_t generation, void *context)
{
    (void)context;
    return xmini_audio_stream_busy(generation);
}

static void media_flush(uint32_t generation, void *context)
{
    (void)context;
    xmini_audio_stream_flush(generation);
}

static void control(void *context)
{
    TaskHandle_t caller = (TaskHandle_t)context;
    const xmini_business_callbacks_t callbacks = {.receive = receive, .media_start = media_start,
        .media_stop = media_stop, .media_busy = media_busy, .media_flush = media_flush};
    int error = xmini_business_init(&callbacks);
    s_init_error = error;
    xTaskNotifyGive(caller);
    if (error) {
        ESP_LOGE("xmini_runtime", "business initialization failed: %d", error);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        xmini_business_process();
        xmini_settings_process();
        wifi_setup_process();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t xmini_runtime_init(void)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    s_wifi_queue = xQueueCreateStatic(1, 1, s_wifi_queue_buffer, &s_wifi_queue_storage);
    if (xTaskCreate(control, "xmini_control", 10240, xTaskGetCurrentTaskHandle(), 4, &s_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000))) return ESP_ERR_TIMEOUT;
    if (s_init_error) s_task = NULL;
    atomic_store(&s_ready, s_init_error == 0);
    return s_init_error;
}

bool xmini_runtime_ready(void) { return atomic_load(&s_ready); }
esp_err_t xmini_runtime_wifi_setup(void)
{
    uint8_t action = 1;
    return atomic_load(&s_ready) && s_wifi_queue && xQueueSend(s_wifi_queue, &action, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}
esp_err_t xmini_runtime_wifi_cancel(void)
{
    uint8_t action = 2;
    return atomic_load(&s_ready) && s_wifi_queue && xQueueSend(s_wifi_queue, &action, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}
int xmini_runtime_wifi_phase(void) { return atomic_load(&s_wifi_phase); }
int xmini_runtime_wifi_error(void) { return atomic_load(&s_wifi_error); }

unsigned xmini_runtime_stack_watermark(void)
{
    return s_task ? (unsigned)uxTaskGetStackHighWaterMark(s_task) : 0;
}
